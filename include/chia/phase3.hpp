/*
 * phase3.hpp
 *
 *  Created on: May 30, 2021
 *      Author: mad
 */

#ifndef INCLUDE_CHIA_PHASE3_HPP_
#define INCLUDE_CHIA_PHASE3_HPP_

#include <chia/chia.h>
#include <chia/phase3.h>
#include <chia/encoding.hpp>
#include <chia/DiskTable.h>

#include <list>


namespace phase3 {

template<typename T, typename S, typename DS_L, typename DS_R>
void compute_stage1(int L_index, int num_threads,
					DS_L* L_sort, DS_R* R_sort, DiskSortLP* R_sort_2,
					DiskTable<T>* L_table = nullptr, bitfield const* L_used = nullptr,
					DiskTable<S>* R_table = nullptr)
{
	const auto begin = get_wall_time_micros();
	
	std::mutex mutex;
	std::condition_variable signal;
	
	bool L_is_end = false;
	bool R_is_waiting = false;
	uint64_t L_offset = 0;				// position offset at L_buffer[0]
	uint64_t L_position = 0;			// lowest position needed
	std::vector<uint32_t> L_buffer;		// new_pos buffer
	std::atomic<uint64_t> R_num_write {0};
	
	Thread<std::vector<entry_np>> L_read(
		[&mutex, &signal, &L_buffer, &L_offset, &L_position, &R_is_waiting]
		 (std::vector<entry_np>& input) {
			std::unique_lock<std::mutex> lock(mutex);
			while(!R_is_waiting) {
				signal.wait(lock);
			}
			if(L_position > L_offset) {
				// delete old data
				const auto count = std::min<uint64_t>(L_position - L_offset, L_buffer.size());
				L_offset += count;
				L_buffer.erase(L_buffer.begin(), L_buffer.begin() + count);
			}
			for(const auto& entry : input) {
				L_buffer.push_back(entry.pos);
			}
			lock.unlock();
			signal.notify_all();
		}, "phase3/buffer");
	
	Thread<std::pair<std::vector<T>, size_t>> L_read_1(
		[L_used, &L_read](std::pair<std::vector<T>, size_t>& input) {
			std::vector<entry_np> out;
			out.reserve(input.first.size());
			size_t offset = 0;
			for(const auto& entry : input.first) {
				if(!L_used->get(input.second + (offset++))) {
					continue;	// drop it
				}
				entry_np tmp;
				tmp.pos = get_new_pos<T>{}(entry);
				out.push_back(tmp);
			}
			L_read.take(out);
		}, "phase3/filter_1");
	
	typedef DiskSortLP::WriteCache WriteCache;
	
	ThreadPool<std::vector<entry_kpp>, size_t, std::shared_ptr<WriteCache>> R_add_2(
		[R_sort_2, &R_num_write]
		 (std::vector<entry_kpp>& input, size_t&, std::shared_ptr<WriteCache>& cache) {
			if(!cache) {
				cache = R_sort_2->add_cache();
			}
			for(auto& entry : input) {
				entry_lp tmp;
				tmp.key = entry.key;
				tmp.point = Encoding::SquareToLinePoint(entry.pos[0], entry.pos[1]);
				cache->add(tmp);
			}
			R_num_write += input.size();
		}, nullptr, std::max(num_threads / 2, 1), "phase3/add");
	
	Thread<std::vector<S>> R_read(
		[&mutex, &signal, &L_offset, &L_position, &L_buffer, &L_is_end, &R_is_waiting, &R_add_2]
		 (std::vector<S>& input) {
			std::vector<entry_kpp> out;
			out.reserve(input.size());
			std::unique_lock<std::mutex> lock(mutex);
			for(const auto& entry : input) {
				uint64_t pos[2];
				pos[0] = entry.pos;
				pos[1] = uint64_t(entry.pos) + entry.off;
				if(pos[0] < L_offset) {
					throw std::logic_error("buffer underflow");
				}
				while(L_buffer.size() <= pos[1] - L_offset && !L_is_end) {
					R_is_waiting = true;
					L_position = pos[0];
					signal.notify_all();
					signal.wait(lock);
					R_is_waiting = false;
				}
				pos[0] -= L_offset;
				pos[1] -= L_offset;
				if(std::max(pos[0], pos[1]) >= L_buffer.size()) {
					throw std::logic_error("position out of bounds (" + std::to_string(L_offset)
						+ " + max(" + std::to_string(pos[0]) + "," + std::to_string(pos[1]) + "))");
				}
				entry_kpp tmp;
				tmp.key = get_sort_key<S>{}(entry);
				tmp.pos[0] = L_buffer[pos[0]];
				tmp.pos[1] = L_buffer[pos[1]];
				out.push_back(tmp);
			}
			R_add_2.take(out);
		}, "phase3/merge");
	
	Thread<std::pair<std::vector<S>, size_t>> R_read_7(
		[&R_read](std::pair<std::vector<S>, size_t>& input) {
			R_read.take(input.first);
		}, "phase3/misc");
	
	std::thread R_sort_read(
		[num_threads, L_table, R_sort, R_table, &R_read, &R_read_7]() {
			if(R_table) {
				R_table->read(&R_read_7, std::max(num_threads / 4, 2));
				R_read_7.close();
			} else {
				R_sort->read(&R_read, std::max(num_threads / (L_table ? 1 : 2), 1));
			}
		});
	
	if(L_table) {
		L_table->read(&L_read_1, std::max(num_threads / 4, 2));
		L_read_1.close();
	} else {
		L_sort->read(&L_read, std::max(num_threads / (R_table ? 1 : 2), 1));
	}
	L_read.close();
	{
		std::lock_guard<std::mutex> lock(mutex);
		L_is_end = true;
		signal.notify_all();
	}
	R_sort_read.join();
	R_read.close();
	R_add_2.close();
	
	R_sort_2->finish();
	
	std::cout << "[P3-1] Table " << L_index + 1 << " took "
				<< (get_wall_time_micros() - begin) / 1e6 << " sec"
				<< ", wrote " << R_num_write << " right entries" << std::endl;
}

static uint32_t CalculateLinePointSize(uint8_t k) {
	return Util::ByteAlign(2 * k) / 8;
}

static uint32_t CalculateStubsSize(uint32_t k) {
	return Util::ByteAlign((kEntriesPerPark - 1) * (k - kStubMinusBits)) / 8;
}

// This is the full size of the deltas section in a park. However, it will not be fully filled
static uint32_t CalculateMaxDeltasSize(uint8_t k, uint8_t table_index)
{
	if (table_index == 1) {
		return Util::ByteAlign((kEntriesPerPark - 1) * kMaxAverageDeltaTable1) / 8;
	}
	return Util::ByteAlign((kEntriesPerPark - 1) * kMaxAverageDelta) / 8;
}

static uint32_t CalculateParkSize(uint8_t k, uint8_t table_index)
{
	return CalculateLinePointSize(k) + CalculateStubsSize(k) +
		   CalculateMaxDeltasSize(k, table_index);
}

// Writes the plot file header to a file
uint32_t WriteHeader(
	FILE* file,
	uint8_t k,
	const uint8_t* id,
	const uint8_t* memo,
	uint32_t memo_len)
{
	// 19 bytes  - "Proof of Space Plot" (utf-8)
	// 32 bytes  - unique plot id
	// 1 byte    - k
	// 2 bytes   - format description length
	// x bytes   - format description
	// 2 bytes   - memo length
	// x bytes   - memo

	const std::string header_text = "Proof of Space Plot";
	
	size_t num_bytes = 0;
	num_bytes += fwrite(header_text.c_str(), 1, header_text.size(), file);
	num_bytes += fwrite((id), 1, kIdLen, file);

	uint8_t k_buffer[1] = {k};
	num_bytes += fwrite((k_buffer), 1, 1, file);

	uint8_t size_buffer[2];
	Util::IntToTwoBytes(size_buffer, kFormatDescription.size());
	num_bytes += fwrite((size_buffer), 1, 2, file);
	num_bytes += fwrite(kFormatDescription.c_str(), 1, kFormatDescription.size(), file);

	Util::IntToTwoBytes(size_buffer, memo_len);
	num_bytes += fwrite((size_buffer), 1, 2, file);
	num_bytes += fwrite((memo), 1, memo_len, file);

	uint8_t pointers[10 * 8] = {};
	num_bytes += fwrite((pointers), 8, 10, file) * 8;

	fflush(file);
	std::cout << "Wrote plot header with " << num_bytes << " bytes" << std::endl;
	return num_bytes;
}

// This writes a number of entries into a file, in the final, optimized format. The park
// contains a checkpoint value (which is a 2k bits line point), as well as EPP (entries per
// park) entries. These entries are each divided into stub and delta section. The stub bits are
// encoded as is, but the delta bits are optimized into a variable encoding scheme. Since we
// have many entries in each park, we can approximate how much space each park with take. Format
// is: [2k bits of first_line_point]  [EPP-1 stubs] [Deltas size] [EPP-1 deltas]....
// [first_line_point] ...
inline
void WritePark(
    uint128_t first_line_point,
    const std::vector<uint8_t>& park_deltas,
    const std::vector<uint64_t>& park_stubs,
    uint8_t table_index,
    uint8_t* park_buffer,
    const uint64_t park_buffer_size)
{
    static constexpr uint8_t k = 32;
    
	// Parks are fixed size, so we know where to start writing. The deltas will not go over
    // into the next park.
    uint8_t* index = park_buffer;

    first_line_point <<= 128 - 2 * k;
    Util::IntTo16Bytes(index, first_line_point);
    index += CalculateLinePointSize(k);

    // We use ParkBits instead of Bits since it allows storing more data
    ParkBits park_stubs_bits;
    for (uint64_t stub : park_stubs) {
        park_stubs_bits.AppendValue(stub, (k - kStubMinusBits));
    }
    uint32_t stubs_size = CalculateStubsSize(k);
    uint32_t stubs_valid_size = cdiv(park_stubs_bits.GetSize(), 8);
    park_stubs_bits.ToBytes(index);
    memset(index + stubs_valid_size, 0, stubs_size - stubs_valid_size);
    index += stubs_size;

    // The stubs are random so they don't need encoding. But deltas are more likely to
    // be small, so we can compress them
    const double R = kRValues[table_index - 1];
    uint8_t* deltas_start = index + 2;
    size_t deltas_size = Encoding::ANSEncodeDeltas(park_deltas, R, deltas_start);

    if (!deltas_size) {
        // Uncompressed
        deltas_size = park_deltas.size();
        Util::IntToTwoBytesLE(index, deltas_size | 0x8000);
        memcpy(deltas_start, park_deltas.data(), deltas_size);
    } else {
        // Compressed
        Util::IntToTwoBytesLE(index, deltas_size);
    }
    index += 2 + deltas_size;

    if ((uint64_t)(index - park_buffer) > park_buffer_size) {
        throw std::logic_error(
            "Overflowed park buffer, writing " + std::to_string(index - park_buffer) +
            " bytes. Space: " + std::to_string(park_buffer_size));
    }
    memset(index, 0x00, park_buffer_size - (index - park_buffer));
}

inline
uint64_t compute_stage2(int L_index, int num_threads,
						DiskSortLP* R_sort, DiskSortNP* L_sort,
						FILE* plot_file, uint64_t L_final_begin, uint64_t* R_final_begin)
{
	const auto begin = get_wall_time_micros();
	
	uint64_t R_num_read = 0;
	std::atomic<uint64_t> L_num_write {0};
	std::atomic<uint64_t> num_written_final {0};
	
	struct park_data_t {
		uint64_t index = 0;
		std::vector<uint64_t> points;
	} park;
	
	struct park_out_t {
		uint64_t offset = 0;
		std::vector<uint8_t> buffer;
	};
	
	const auto park_size_bytes = CalculateParkSize(32, L_index);
	
	typedef DiskSortNP::WriteCache WriteCache;
	
	ThreadPool<std::vector<entry_np>, size_t, std::shared_ptr<WriteCache>> L_add(
		[L_sort, &L_num_write]
		 (std::vector<entry_np>& input, size_t&, std::shared_ptr<WriteCache>& cache) {
			if(!cache) {
				cache = L_sort->add_cache();
			}
			for(auto& entry : input) {
				cache->add(entry);
			}
			L_num_write += input.size();
		}, nullptr, std::max(num_threads / 2, 1), "phase3/add");
	
	Thread<park_out_t> park_write(
		[plot_file](park_out_t& park) {
			fwrite_at(plot_file, park.offset, park.buffer.data(), park.buffer.size());
		}, "phase3/write");
	
	ThreadPool<park_data_t, park_out_t> park_threads(
		[L_index, L_final_begin, park_size_bytes, &num_written_final]
		 (park_data_t& input, park_out_t& out, size_t&) {
			const auto& points = input.points;
			if(points.empty()) {
				throw std::logic_error("empty park input");
			}
			std::vector<uint8_t> deltas(points.size() - 1);
			std::vector<uint64_t> stubs(points.size() - 1);
			for(size_t i = 0; i < points.size() - 1; ++i) {
				const auto big_delta = points[i + 1] - points[i];
				const auto stub = big_delta & ((1ull << (32 - kStubMinusBits)) - 1);
				const auto small_delta = big_delta >> (32 - kStubMinusBits);
				if(small_delta >= 256) {
					throw std::logic_error("small_delta >= 256 (" + std::to_string(small_delta) + ")");
				}
				deltas[i] = small_delta;
				stubs[i] = stub;
			}
			out.offset = L_final_begin + input.index * park_size_bytes;
			out.buffer.resize(park_size_bytes);
			WritePark(
				points[0],
				deltas,
				stubs,
				L_index,
				out.buffer.data(),
				out.buffer.size());
			num_written_final += points.size();
		}, &park_write, std::max(num_threads / 2, 1), "phase3/park");
	
	Thread<std::vector<entry_lp>> R_read(
		[&R_num_read, &L_add, &park, &park_threads]
		 (std::vector<entry_lp>& input) {
			std::vector<entry_np> out;
			out.reserve(input.size());
			for(const auto& entry : input) {
				const auto index = R_num_read++;
				if(index >= uint64_t(1) << 32) {
					continue;	// skip 32-bit overflow
				}
				entry_np tmp;
				tmp.key = entry.key;
				tmp.pos = index;
				out.push_back(tmp);
				
				// Every EPP entries, writes a park
				if(index % kEntriesPerPark == 0) {
					if(index != 0) {
						park_threads.take(park);
						park.index++;
					}
					park.points.clear();
					park.points.reserve(kEntriesPerPark);
				}
				park.points.push_back(entry.point);
			}
			L_add.take(out);
		}, "phase3/slice");
	
	R_sort->read(&R_read, num_threads);
	R_read.close();
	
	// Since we don't have a perfect multiple of EPP entries, this writes the last ones
	if(!park.points.empty()) {
		park_threads.take(park);
		park.index++;
	}
	park_threads.close();
	park_write.close();
	L_add.close();
	
	L_sort->finish();
	
	if(R_final_begin) {
		*R_final_begin = L_final_begin + park.index * park_size_bytes;
	}
	Encoding::ANSFree(kRValues[L_index - 1]);
	
	if(L_num_write < R_num_read) {
//		std::cout << "[P3-2] Lost " << R_num_read - L_num_write << " entries due to 32-bit overflow." << std::endl;
	}
	std::cout << "[P3-2] Table " << L_index + 1 << " took "
				<< (get_wall_time_micros() - begin) / 1e6 << " sec"
				<< ", wrote " << L_num_write << " left entries"
				<< ", " << num_written_final << " final" << std::endl;
	return num_written_final;
}

inline
void compute(	phase2::output_t& input, output_t& out,
				const int num_threads, const int log_num_buckets,
				const std::string plot_name,
				const std::string tmp_dir,
				const std::string tmp_dir_2)
{
	const auto total_begin = get_wall_time_micros();
	
	const std::string prefix_2 = tmp_dir_2 + plot_name + ".";
	
	out.params = input.params;
	out.plot_file_name = tmp_dir + plot_name + ".plot.tmp";
	
	FILE* plot_file = fopen(out.plot_file_name.c_str(), "wb");
	if(!plot_file) {
		throw std::runtime_error("fopen() failed");
	}
	out.header_size = WriteHeader(	plot_file, 32, input.params.id.data(),
									input.params.memo.data(), input.params.memo.size());
	
	std::vector<uint64_t> final_pointers(8, 0);
	final_pointers[1] = out.header_size;
	
	uint64_t num_written_final = 0;
	
	DiskTable<phase2::entry_1> L_table_1(input.table_1);
	
	auto R_sort_lp = std::make_shared<DiskSortLP>(
			63, log_num_buckets, prefix_2 + "p3s1.t2");
	
	compute_stage1<phase2::entry_1, phase2::entry_x, DiskSortNP, phase2::DiskSortT>(
			1, num_threads, nullptr, input.sort[1].get(), R_sort_lp.get(), &L_table_1, input.bitfield_1.get());
	
	input.bitfield_1 = nullptr;
	remove(input.table_1.file_name);
	
	auto L_sort_np = std::make_shared<DiskSortNP>(
			32, log_num_buckets, prefix_2 + "p3s2.t2");
	
	num_written_final += compute_stage2(
			1, num_threads, R_sort_lp.get(), L_sort_np.get(),
			plot_file, final_pointers[1], &final_pointers[2]);
	
	for(int L_index = 2; L_index < 6; ++L_index)
	{
		const std::string R_t = "t" + std::to_string(L_index + 1);
		
		R_sort_lp = std::make_shared<DiskSortLP>(
				63, log_num_buckets, prefix_2 + "p3s1." + R_t);
		
		compute_stage1<entry_np, phase2::entry_x, DiskSortNP, phase2::DiskSortT>(
				L_index, num_threads, L_sort_np.get(), input.sort[L_index].get(), R_sort_lp.get());
		
		L_sort_np = std::make_shared<DiskSortNP>(
				32, log_num_buckets, prefix_2 + "p3s2." + R_t);
		
		num_written_final += compute_stage2(
				L_index, num_threads, R_sort_lp.get(), L_sort_np.get(),
				plot_file, final_pointers[L_index], &final_pointers[L_index + 1]);
	}
	
	DiskTable<phase2::entry_7> R_table_7(input.table_7);
	
	R_sort_lp = std::make_shared<DiskSortLP>(63, log_num_buckets, prefix_2 + "p3s1.t7");
	
	compute_stage1<entry_np, phase2::entry_7, DiskSortNP, phase2::DiskSort7>(
			6, num_threads, L_sort_np.get(), nullptr, R_sort_lp.get(), nullptr, nullptr, &R_table_7);
	
	remove(input.table_7.file_name);
	
	L_sort_np = std::make_shared<DiskSortNP>(32, log_num_buckets, prefix_2 + "p3s2.t7");
	
	const auto num_written_final_7 = compute_stage2(
			6, num_threads, R_sort_lp.get(), L_sort_np.get(),
			plot_file, final_pointers[6], &final_pointers[7]);
	num_written_final += num_written_final_7;
	
	fseek_set(plot_file, out.header_size - 10 * 8);
	for(size_t i = 1; i < final_pointers.size(); ++i) {
		uint8_t tmp[8] = {};
		Util::IntToEightBytes(tmp, final_pointers[i]);
		fwrite_ex(plot_file, tmp, sizeof(tmp));
	}
	fclose(plot_file);
	
	out.sort_7 = L_sort_np;
	out.num_written_7 = num_written_final_7;
	out.final_pointer_7 = final_pointers[7];
	
	std::cout << "Phase 3 took " << (get_wall_time_micros() - total_begin) / 1e6 << " sec"
			", wrote " << num_written_final << " entries to final plot" << std::endl;
}


} // phase3

#endif /* INCLUDE_CHIA_PHASE3_HPP_ */
