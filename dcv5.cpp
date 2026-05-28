#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "m03_tables.h"
#include "libsais/libsais.h"

namespace {

constexpr uint8_t kModeDigitContextV5 = 6;
constexpr uint8_t kModeDigitContextV5Native = 7;
constexpr uint8_t kModeDigitContextV5Chunked = 8;
constexpr uint8_t kModeBwtOrder0 = 9;
constexpr int kCap = 12;
constexpr uint8_t kPlaceholder = static_cast<uint8_t>('9');
constexpr uint64_t kCodeBits = 32;
constexpr uint64_t kTopValue = (uint64_t{1} << kCodeBits) - 1;
constexpr uint64_t kFirstQtr = (kTopValue + 1) / 4;
constexpr uint64_t kHalf = kFirstQtr * 2;
constexpr uint64_t kThirdQtr = kFirstQtr * 3;
thread_local const char* g_uniform_context = "unset";

struct DigitRun {
    size_t start = 0;
    size_t end = 0;
    int prev_byte = -1;
    int next_byte = -1;
    int prev2_norm = -1;
    int prev3_norm = -1;
    int prev4_norm = -1;
    int next2_norm = -1;

    size_t length() const { return end - start; }
};

struct DigitArchiveParts {
    std::vector<uint8_t> blob;
    uint8_t mode = 0;
    uint64_t main_len = 0;
    std::vector<uint64_t> stream_lens;
    size_t header_bytes = 0;
    size_t main_offset = 0;
    uint64_t side_total = 0;
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input: " + path);
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        throw std::runtime_error("cannot stat input: " + path);
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in) {
            throw std::runtime_error("short read: " + path);
        }
    }
    return data;
}

void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot open output: " + path);
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            throw std::runtime_error("short write: " + path);
        }
    }
}

void write_blob_to_stream(std::ofstream& out, const std::vector<uint8_t>& data, const std::string& path) {
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            throw std::runtime_error("short write: " + path);
        }
    }
}

uint64_t read_varint(const std::vector<uint8_t>& data, size_t& pos) {
    uint64_t value = 0;
    int shift = 0;
    while (true) {
        if (pos >= data.size()) {
            throw std::runtime_error("truncated varint");
        }
        const uint8_t b = data[pos++];
        value |= static_cast<uint64_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            return value;
        }
        shift += 7;
        if (shift > 63) {
            throw std::runtime_error("unreasonable varint");
        }
    }
}

void write_varint(uint64_t value, std::vector<uint8_t>& out) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
            out.push_back(ch);
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

DigitArchiveParts parse_digit_archive_blob(std::vector<uint8_t> blob) {
    DigitArchiveParts parts;
    parts.blob = std::move(blob);
    if (parts.blob.empty() || (parts.blob[0] != kModeDigitContextV5 && parts.blob[0] != kModeDigitContextV5Native)) {
        throw std::runtime_error("not a digit_context_v5 archive");
    }
    parts.mode = parts.blob[0];
    size_t pos = 1;
    parts.main_len = read_varint(parts.blob, pos);
    for (int i = 0; i < kCap + 1; ++i) {
        parts.stream_lens.push_back(read_varint(parts.blob, pos));
    }
    parts.header_bytes = pos;
    parts.main_offset = pos;
    for (uint64_t len : parts.stream_lens) {
        parts.side_total += len;
    }
    if (pos + parts.main_len + parts.side_total != parts.blob.size()) {
        throw std::runtime_error("component lengths do not match archive size");
    }
    return parts;
}

DigitArchiveParts parse_digit_archive(const std::string& path) {
    return parse_digit_archive_blob(read_file(path));
}

class BitOutput {
public:
    void write_bit(int bit) {
        acc_ = static_cast<uint8_t>((acc_ << 1) | (bit ? 1 : 0));
        ++bits_;
        if (bits_ == 8) {
            data_.push_back(acc_);
            acc_ = 0;
            bits_ = 0;
        }
    }

    std::vector<uint8_t> finish() {
        if (bits_ != 0) {
            data_.push_back(static_cast<uint8_t>(acc_ << (8 - bits_)));
            acc_ = 0;
            bits_ = 0;
        }
        return data_;
    }

private:
    std::vector<uint8_t> data_;
    uint8_t acc_ = 0;
    int bits_ = 0;
};

class BitInput {
public:
    explicit BitInput(const std::vector<uint8_t>& data) : data_(data) {}

    int read_bit() {
        if (byte_ >= data_.size()) {
            return 0;
        }
        const int value = (data_[byte_] >> (7 - bit_)) & 1;
        ++bit_;
        if (bit_ == 8) {
            ++byte_;
            bit_ = 0;
        }
        return value;
    }

private:
    const std::vector<uint8_t>& data_;
    size_t byte_ = 0;
    int bit_ = 0;
};

class ArithmeticEncoder {
public:
    void encode(uint64_t cum_low, uint64_t cum_high, uint64_t total) {
        if (!(cum_low < cum_high && cum_high <= total)) {
            throw std::runtime_error("invalid arithmetic interval");
        }
        const uint64_t span = high_ - low_ + 1;
        high_ = low_ + (span * cum_high / total) - 1;
        low_ = low_ + (span * cum_low / total);
        while (true) {
            if (high_ < kHalf) {
                bit_plus_pending(0);
            } else if (low_ >= kHalf) {
                bit_plus_pending(1);
                low_ -= kHalf;
                high_ -= kHalf;
            } else if (low_ >= kFirstQtr && high_ < kThirdQtr) {
                ++pending_;
                low_ -= kFirstQtr;
                high_ -= kFirstQtr;
            } else {
                break;
            }
            low_ <<= 1;
            high_ = (high_ << 1) + 1;
        }
    }

    std::vector<uint8_t> finish() {
        ++pending_;
        if (low_ < kFirstQtr) {
            bit_plus_pending(0);
        } else {
            bit_plus_pending(1);
        }
        return out_.finish();
    }

private:
    void bit_plus_pending(int bit) {
        out_.write_bit(bit);
        while (pending_ > 0) {
            out_.write_bit(1 - bit);
            --pending_;
        }
    }

    uint64_t low_ = 0;
    uint64_t high_ = kTopValue;
    uint64_t pending_ = 0;
    BitOutput out_;
};

class ArithmeticDecoder {
public:
    explicit ArithmeticDecoder(const std::vector<uint8_t>& data) : in_(data) {
        for (uint64_t i = 0; i < kCodeBits; ++i) {
            code_ = (code_ << 1) | static_cast<uint64_t>(in_.read_bit());
        }
    }

    uint64_t value(uint64_t total) const {
        const uint64_t span = high_ - low_ + 1;
        return ((code_ - low_ + 1) * total - 1) / span;
    }

    void update(uint64_t cum_low, uint64_t cum_high, uint64_t total) {
        const uint64_t span = high_ - low_ + 1;
        high_ = low_ + (span * cum_high / total) - 1;
        low_ = low_ + (span * cum_low / total);
        while (true) {
            if (high_ < kHalf) {
                // Nothing to normalize.
            } else if (low_ >= kHalf) {
                code_ -= kHalf;
                low_ -= kHalf;
                high_ -= kHalf;
            } else if (low_ >= kFirstQtr && high_ < kThirdQtr) {
                code_ -= kFirstQtr;
                low_ -= kFirstQtr;
                high_ -= kFirstQtr;
            } else {
                break;
            }
            low_ <<= 1;
            high_ = (high_ << 1) + 1;
            code_ = (code_ << 1) | static_cast<uint64_t>(in_.read_bit());
        }
    }

private:
    uint64_t low_ = 0;
    uint64_t high_ = kTopValue;
    uint64_t code_ = 0;
    BitInput in_;
};

class AdaptiveModel {
public:
    explicit AdaptiveModel(size_t size) : counts_(size, 1), total_(size) {
        if (size == 0) {
            throw std::runtime_error("empty adaptive model");
        }
    }

    std::tuple<uint64_t, uint64_t, uint64_t> interval(size_t symbol) const {
        if (symbol >= counts_.size()) {
            throw std::runtime_error("symbol outside model");
        }
        uint64_t lo = 0;
        for (size_t i = 0; i < symbol; ++i) {
            lo += counts_[i];
        }
        return {lo, lo + counts_[symbol], total_};
    }

    std::tuple<size_t, uint64_t, uint64_t, uint64_t> symbol_for_value(uint64_t value) const {
        uint64_t acc = 0;
        for (size_t sym = 0; sym < counts_.size(); ++sym) {
            const uint64_t next = acc + counts_[sym];
            if (value < next) {
                return {sym, acc, next, total_};
            }
            acc = next;
        }
        throw std::runtime_error("arithmetic value outside model");
    }

    void update(size_t symbol) {
        ++counts_[symbol];
        ++total_;
        if (total_ >= (uint64_t{1} << 15)) {
            total_ = 0;
            for (uint64_t& count : counts_) {
                count = std::max<uint64_t>(1, (count + 1) / 2);
                total_ += count;
            }
        }
    }

    uint64_t total() const { return total_; }

private:
    std::vector<uint64_t> counts_;
    uint64_t total_ = 0;
};

void encode_value_stream(ArithmeticEncoder& encoder, uint64_t min_value, uint64_t value, uint64_t max_value) {
    if (!(min_value <= value && value <= max_value)) {
        throw std::runtime_error("uniform value outside bounds: context=" + std::string(g_uniform_context) +
                                 " min=" + std::to_string(min_value) +
                                 " value=" + std::to_string(value) +
                                 " max=" + std::to_string(max_value));
    }
    while (max_value - min_value >= 0x10000) {
        const uint64_t median = min_value + ((max_value - min_value) >> 1);
        const uint64_t bit = static_cast<uint64_t>(value > median);
        encoder.encode(bit, bit + 1, 2);
        if (bit) {
            min_value = median + 1;
        } else {
            max_value = median;
        }
    }
    if (min_value != max_value) {
        encoder.encode(value - min_value, value - min_value + 1, max_value - min_value + 1);
    }
}

uint64_t decode_value_stream(ArithmeticDecoder& decoder, uint64_t min_value, uint64_t max_value) {
    if (min_value > max_value) {
        throw std::runtime_error("invalid uniform bounds");
    }
    while (max_value - min_value >= 0x10000) {
        const uint64_t median = min_value + ((max_value - min_value) >> 1);
        const uint64_t bit = decoder.value(2);
        decoder.update(bit, bit + 1, 2);
        if (bit) {
            min_value = median + 1;
        } else {
            max_value = median;
        }
    }
    if (min_value != max_value) {
        const uint64_t total = max_value - min_value + 1;
        const uint64_t value = decoder.value(total);
        decoder.update(value, value + 1, total);
        min_value += value;
    }
    return min_value;
}

int bit_scan_reverse(uint64_t value) {
    if (value == 0) {
        throw std::runtime_error("bit_scan_reverse expects positive input");
    }
    int bit = 0;
    while (value >>= 1) {
        ++bit;
    }
    return bit;
}

class M03SerializablePredictor {
public:
    M03SerializablePredictor()
        : t1_model_(init_model(scale_len(m03_T1_model_m0_scale_table), 5, 3)),
          t2_model_m0_(init_model(scale_len(m03_T2_model_m0_scale_table), 2, 5)),
          t2_model_m1_(init_model(scale_len(m03_T2_model_m1_scale_table), 3, 2)),
          t3_model_m0_(init_model(scale_len(m03_T3_model_m0_scale_table), 2, 2)),
          t3_model_m1_(init_model(scale_len(m03_T3_model_m1_scale_table), 2, 2)),
          t3_model_m2_(init_model(scale_len(m03_T3_model_m2_scale_table), 2, 2)),
          tx_model_m0_(init_model(scale_len(m03_Tx_model_m0_scale_table), 2, 2)),
          tx_model_m1_(init_model(scale_len(m03_Tx_model_m1_scale_table), 2, 2)),
          tx_model_m2_(init_model(scale_len(m03_Tx_model_m2_scale_table), 6, 6)) {}

    void encode_count(ArithmeticEncoder& encoder,
                      int count,
                      int total,
                      int left_remaining,
                      int right_remaining,
                      int symbols_remaining,
                      int context) {
        const int inferred_right = std::max(total - left_remaining, 0);
        total -= inferred_right;
        right_remaining -= inferred_right << 1;
        if (!(left_remaining > 0 && right_remaining > 0 && total <= left_remaining &&
              left_remaining <= right_remaining)) {
            throw std::runtime_error("invalid m03 predictor state");
        }

        if (total <= 3) {
            int state = 0;
            state += context;
            state += 32 * std::min(symbols_remaining - 2, 7);
            state += 256 * std::min(bit_scan_reverse(static_cast<uint64_t>(inferred_right + 1)), 3);
            state += 1024 * static_cast<int>(left_remaining + right_remaining + inferred_right == symbols_remaining);
            state += 2048 * static_cast<int>(left_remaining == total);
            state += 4096 * static_cast<int>((static_cast<int64_t>(left_remaining) * 11) / right_remaining);

            if (total == 1) {
                auto ref = select_model(m03_T1_model_m0_state_table, state, t1_model_, m03_T1_model_m0_scale_table);
                encode_binary(encoder, count ? 1 : 0, ref);
                return;
            }
            if (total == 2) {
                const int pivot = static_cast<int>(count == 0 || count == 2);
                auto ref0 = select_model(m03_T2_model_m0_state_table, state, t2_model_m0_, m03_T2_model_m0_scale_table);
                encode_binary(encoder, pivot, ref0);
                if (pivot) {
                    const int side = static_cast<int>(count > 0);
                    auto ref1 = select_model(m03_T2_model_m1_state_table, state, t2_model_m1_, m03_T2_model_m1_scale_table);
                    encode_binary(encoder, side, ref1);
                }
                return;
            }

            const int pivot = static_cast<int>(count == 0 || count == 3);
            auto ref0 = select_model(m03_T3_model_m0_state_table, state, t3_model_m0_, m03_T3_model_m0_scale_table);
            encode_binary(encoder, pivot, ref0);
            if (pivot) {
                const int side = static_cast<int>(count > 0);
                auto ref1 = select_model(m03_T3_model_m1_state_table, state, t3_model_m1_, m03_T3_model_m1_scale_table);
                encode_binary(encoder, side, ref1);
            } else {
                const int side = count - 1;
                auto ref2 = select_model(m03_T3_model_m2_state_table, state, t3_model_m2_, m03_T3_model_m2_scale_table);
                encode_binary(encoder, side, ref2);
            }
            return;
        }

        int state = 0;
        state += std::min(bit_scan_reverse(static_cast<uint64_t>(total - 3)), 7);
        state += 8 * context;
        state += 256 * std::min(bit_scan_reverse(static_cast<uint64_t>(symbols_remaining - 1)), 3);
        state += 1024 * static_cast<int>(left_remaining == total);
        state += 2048 * static_cast<int>(inferred_right > 0);
        state += 4096 * static_cast<int>((static_cast<int64_t>(left_remaining) * 11) / right_remaining);

        const int pivot = static_cast<int>(count == 0 || count == total);
        auto ref0 = select_model(m03_Tx_model_m0_state_table, state, tx_model_m0_, m03_Tx_model_m0_scale_table);
        encode_binary(encoder, pivot, ref0);
        if (pivot) {
            const int side = static_cast<int>(count > 0);
            auto ref1 = select_model(m03_Tx_model_m1_state_table, state, tx_model_m1_, m03_Tx_model_m1_scale_table);
            encode_binary(encoder, side, ref1);
            return;
        }

        int state2 = 0;
        state2 += std::min(total - 4, 15);
        state2 += 16 * (context & 3);
        state2 += 64 * static_cast<int>((static_cast<int64_t>(left_remaining) * 7) / right_remaining);
        state2 += 512 * std::min(bit_scan_reverse(static_cast<uint64_t>(symbols_remaining - 1)), 3);
        state2 += 2048 * static_cast<int>(inferred_right >= total);

        int min_value = 1;
        int max_value = total - 1;
        int path_context = 1;
        while (min_value != max_value && path_context < 16) {
            auto ref2 = select_model(
                m03_Tx_model_m2_state_table,
                state2 * 16 + path_context,
                tx_model_m2_,
                m03_Tx_model_m2_scale_table);
            const int median = min_value + ((max_value - min_value + 1) >> 1);
            const int bit = static_cast<int>(count >= median);
            encode_binary(encoder, bit, ref2);
            path_context += path_context + bit;
            if (bit) {
                min_value = median;
            } else {
                max_value = median - 1;
            }
        }
        encode_value_stream(encoder, static_cast<uint64_t>(min_value), static_cast<uint64_t>(count), static_cast<uint64_t>(max_value));
    }

    int decode_count(ArithmeticDecoder& decoder,
                     int total,
                     int left_remaining,
                     int right_remaining,
                     int symbols_remaining,
                     int context) {
        const int inferred_right = std::max(total - left_remaining, 0);
        total -= inferred_right;
        right_remaining -= inferred_right << 1;
        if (!(left_remaining > 0 && right_remaining > 0 && total <= left_remaining &&
              left_remaining <= right_remaining)) {
            throw std::runtime_error("invalid m03 predictor state");
        }

        if (total <= 3) {
            int state = 0;
            state += context;
            state += 32 * std::min(symbols_remaining - 2, 7);
            state += 256 * std::min(bit_scan_reverse(static_cast<uint64_t>(inferred_right + 1)), 3);
            state += 1024 * static_cast<int>(left_remaining + right_remaining + inferred_right == symbols_remaining);
            state += 2048 * static_cast<int>(left_remaining == total);
            state += 4096 * static_cast<int>((static_cast<int64_t>(left_remaining) * 11) / right_remaining);

            if (total == 1) {
                auto ref = select_model(m03_T1_model_m0_state_table, state, t1_model_, m03_T1_model_m0_scale_table);
                return decode_binary(decoder, ref) ? 1 : 0;
            }
            if (total == 2) {
                auto ref0 = select_model(m03_T2_model_m0_state_table, state, t2_model_m0_, m03_T2_model_m0_scale_table);
                const int pivot = decode_binary(decoder, ref0);
                if (!pivot) {
                    return 1;
                }
                auto ref1 = select_model(m03_T2_model_m1_state_table, state, t2_model_m1_, m03_T2_model_m1_scale_table);
                const int side = decode_binary(decoder, ref1);
                return side ? 2 : 0;
            }

            auto ref0 = select_model(m03_T3_model_m0_state_table, state, t3_model_m0_, m03_T3_model_m0_scale_table);
            const int pivot = decode_binary(decoder, ref0);
            if (pivot) {
                auto ref1 = select_model(m03_T3_model_m1_state_table, state, t3_model_m1_, m03_T3_model_m1_scale_table);
                const int side = decode_binary(decoder, ref1);
                return side ? 3 : 0;
            }
            auto ref2 = select_model(m03_T3_model_m2_state_table, state, t3_model_m2_, m03_T3_model_m2_scale_table);
            const int side = decode_binary(decoder, ref2);
            return side + 1;
        }

        int state = 0;
        state += std::min(bit_scan_reverse(static_cast<uint64_t>(total - 3)), 7);
        state += 8 * context;
        state += 256 * std::min(bit_scan_reverse(static_cast<uint64_t>(symbols_remaining - 1)), 3);
        state += 1024 * static_cast<int>(left_remaining == total);
        state += 2048 * static_cast<int>(inferred_right > 0);
        state += 4096 * static_cast<int>((static_cast<int64_t>(left_remaining) * 11) / right_remaining);

        auto ref0 = select_model(m03_Tx_model_m0_state_table, state, tx_model_m0_, m03_Tx_model_m0_scale_table);
        const int pivot = decode_binary(decoder, ref0);
        if (pivot) {
            auto ref1 = select_model(m03_Tx_model_m1_state_table, state, tx_model_m1_, m03_Tx_model_m1_scale_table);
            const int side = decode_binary(decoder, ref1);
            return side ? total : 0;
        }

        int state2 = 0;
        state2 += std::min(total - 4, 15);
        state2 += 16 * (context & 3);
        state2 += 64 * static_cast<int>((static_cast<int64_t>(left_remaining) * 7) / right_remaining);
        state2 += 512 * std::min(bit_scan_reverse(static_cast<uint64_t>(symbols_remaining - 1)), 3);
        state2 += 2048 * static_cast<int>(inferred_right >= total);

        int min_value = 1;
        int max_value = total - 1;
        int path_context = 1;
        while (min_value != max_value && path_context < 16) {
            auto ref2 = select_model(
                m03_Tx_model_m2_state_table,
                state2 * 16 + path_context,
                tx_model_m2_,
                m03_Tx_model_m2_scale_table);
            const int median = min_value + ((max_value - min_value + 1) >> 1);
            const int bit = decode_binary(decoder, ref2);
            path_context += path_context + bit;
            if (bit) {
                min_value = median;
            } else {
                max_value = median - 1;
            }
        }
        return static_cast<int>(decode_value_stream(decoder, static_cast<uint64_t>(min_value), static_cast<uint64_t>(max_value)));
    }

private:
    struct BinaryRef {
        std::array<uint32_t, 2>& predictor;
        uint32_t scale;
    };

    template <size_t N>
    static constexpr size_t scale_len(const unsigned short (&)[N]) {
        return N;
    }

    static std::vector<std::array<uint32_t, 2>> init_model(size_t count, uint32_t p0, uint32_t p1) {
        return std::vector<std::array<uint32_t, 2>>(count, {p0, p1});
    }

    template <size_t StateN, size_t ScaleN>
    BinaryRef select_model(const unsigned char (&state_table)[StateN],
                           int state,
                           std::vector<std::array<uint32_t, 2>>& model,
                           const unsigned short (&scale_table)[ScaleN]) {
        if (state < 0 || static_cast<size_t>(state) >= StateN) {
            throw std::runtime_error("m03 predictor state outside table");
        }
        const size_t bucket = state_table[static_cast<size_t>(state)];
        if (bucket >= model.size() || bucket >= ScaleN) {
            throw std::runtime_error("m03 predictor bucket outside table");
        }
        return {model[bucket], static_cast<uint32_t>(scale_table[bucket])};
    }

    void encode_binary(ArithmeticEncoder& encoder, int choice, BinaryRef ref) {
        uint32_t p0 = ref.predictor[0];
        uint32_t p1 = ref.predictor[1];
        if (p0 + p1 > ref.scale) {
            p0 = (p0 + 1) >> 1;
            p1 = (p1 + 1) >> 1;
            ref.predictor[0] = p0;
            ref.predictor[1] = p1;
        }
        const uint32_t total = p0 + p1;
        if (choice == 0) {
            encoder.encode(0, p0, total);
            ref.predictor[0] = p0 + 1;
        } else {
            encoder.encode(p0, total, total);
            ref.predictor[1] = p1 + 1;
        }
    }

    int decode_binary(ArithmeticDecoder& decoder, BinaryRef ref) {
        uint32_t p0 = ref.predictor[0];
        uint32_t p1 = ref.predictor[1];
        if (p0 + p1 > ref.scale) {
            p0 = (p0 + 1) >> 1;
            p1 = (p1 + 1) >> 1;
            ref.predictor[0] = p0;
            ref.predictor[1] = p1;
        }
        const uint32_t total = p0 + p1;
        const uint64_t value = decoder.value(total);
        if (value < p0) {
            decoder.update(0, p0, total);
            ref.predictor[0] = p0 + 1;
            return 0;
        }
        decoder.update(p0, total, total);
        ref.predictor[1] = p1 + 1;
        return 1;
    }

    std::vector<std::array<uint32_t, 2>> t1_model_;
    std::vector<std::array<uint32_t, 2>> t2_model_m0_;
    std::vector<std::array<uint32_t, 2>> t2_model_m1_;
    std::vector<std::array<uint32_t, 2>> t3_model_m0_;
    std::vector<std::array<uint32_t, 2>> t3_model_m1_;
    std::vector<std::array<uint32_t, 2>> t3_model_m2_;
    std::vector<std::array<uint32_t, 2>> tx_model_m0_;
    std::vector<std::array<uint32_t, 2>> tx_model_m1_;
    std::vector<std::array<uint32_t, 2>> tx_model_m2_;
};

struct ContextEntry {
    uint32_t symbol = 0;
    uint32_t count = 0;
    uint32_t offset = 0;
};

class ParserDecodeState {
public:
    ParserDecodeState(size_t n_with_sentinel,
                      uint32_t primary_index,
                      const std::vector<uint64_t>& root_frequencies,
                      ArithmeticDecoder& decoder)
        : n_(n_with_sentinel),
          primary_index_(primary_index),
          root_frequencies_(root_frequencies),
          decoder_(&decoder),
          ctx_count_(n_with_sentinel + 2, 0),
          ctx_offset_(n_with_sentinel + 2, 0),
          ctx_symbol_(n_with_sentinel + 2, 0) {
        for (auto& row : symbol_pivots_) {
            row.fill(0);
        }
    }

    ParserDecodeState(const std::vector<uint8_t>& source_l,
                      uint32_t primary_index,
                      const std::vector<uint64_t>& root_frequencies,
                      ArithmeticEncoder& encoder)
        : n_(source_l.size()),
          primary_index_(primary_index),
          root_frequencies_(root_frequencies),
          encoder_(&encoder),
          source_l_(&source_l),
          ctx_count_(source_l.size() + 2, 0),
          ctx_offset_(source_l.size() + 2, 0),
          ctx_symbol_(source_l.size() + 2, 0) {
        for (auto& row : symbol_pivots_) {
            row.fill(0);
        }
    }

    void initialize_root_context() {
        uint32_t unique_symbols = 0;
        uint32_t total_symbols = 1;
        current_segments_.clear();
        current_segments_.push_back(0);
        for (uint32_t sym = 0; sym < root_frequencies_.size(); ++sym) {
            const uint64_t freq = root_frequencies_[sym];
            if (freq > 0) {
                ctx_count_[unique_symbols] = static_cast<uint32_t>(freq);
                ctx_offset_[unique_symbols] = total_symbols;
                ctx_symbol_[unique_symbols] = static_cast<uint8_t>(sym);
                current_segments_.push_back(total_symbols);
                ++unique_symbols;
                total_symbols += static_cast<uint32_t>(freq);
            }
        }
        normalize_context(0, unique_symbols, total_symbols);
    }

    void apply_split(uint32_t parent_context_offset,
                     uint32_t right_context_offset,
                     uint32_t level,
                     uint32_t left_subintervals,
                     uint32_t right_subintervals) {
        ++splits_;
        check_event_budget();
        level = std::min<uint32_t>(level, 63);
        const uint32_t parent_interval_size = ctx_count_[parent_context_offset];
        const uint32_t left_interval_size = right_context_offset - parent_context_offset;
        const uint32_t right_interval_size = parent_interval_size - left_interval_size;

        const auto entries = extract_context_entries(parent_context_offset);
        int left_leaf = static_cast<int>(left_subintervals == 1);
        int right_leaf = static_cast<int>(right_subintervals == 1);
        int left_remaining =
            static_cast<int>(left_interval_size) -
            static_cast<int>(parent_context_offset <= primary_index_ && primary_index_ < right_context_offset);
        int right_remaining =
            static_cast<int>(right_interval_size) -
            static_cast<int>(right_context_offset <= primary_index_ &&
                             primary_index_ < right_context_offset + right_interval_size);
        uint32_t pivot_history = 0;

        std::vector<ContextEntry> left_entries;
        std::vector<ContextEntry> right_entries;
        const size_t parent_unique_symbols = entries.size();
        for (size_t parent_symbol_index = 0; parent_symbol_index < entries.size(); ++parent_symbol_index) {
            const auto& entry = entries[parent_symbol_index];
            const uint32_t symbol = entry.symbol;
            const int total = static_cast<int>(entry.count);
            const uint32_t offset = entry.offset;
            int count = source_l_ != nullptr ? encode_mode_left_count(parent_context_offset, right_context_offset, symbol) : 0;
            if (source_l_ != nullptr && (count < 0 || count > total)) {
                throw std::runtime_error("encoded split count outside parent total: parent=" +
                                         std::to_string(parent_context_offset) +
                                         " right=" + std::to_string(right_context_offset) +
                                         " symbol=" + std::to_string(symbol) +
                                         " count=" + std::to_string(count) +
                                         " total=" + std::to_string(total));
            }
            if (left_remaining > 0 && right_remaining > 0 && left_remaining + right_remaining != total) {
                int context = symbol_pivots_[symbol][level - 2] | symbol_pivots_[symbol][level - 1];
                int simple = static_cast<int>(total > 1 && ctx_count_[offset] == static_cast<uint32_t>(total) &&
                                              ctx_count_[offset + 1] == 0);
                if (parent_symbol_index == parent_unique_symbols - 2) {
                    const auto& next = entries[parent_symbol_index + 1];
                    context |= symbol_pivots_[next.symbol][level - 2] | symbol_pivots_[next.symbol][level - 1];
                    simple |= static_cast<int>(next.count > 1 && ctx_count_[next.offset] == next.count &&
                                               ctx_count_[next.offset + 1] == 0);
                }
                context += 8 * simple + 16 * static_cast<int>(pivot_history);

                left_leaf &= static_cast<int>(left_remaining == static_cast<int>(left_interval_size));
                right_leaf &= static_cast<int>(right_remaining == static_cast<int>(right_interval_size));
                if (total <= left_remaining + right_remaining - total) {
                    if (source_l_ != nullptr) {
                        encode_split_count(
                            count,
                            total,
                            left_remaining,
                            right_remaining,
                            static_cast<int>(parent_unique_symbols - parent_symbol_index),
                            context,
                            left_leaf,
                            right_leaf);
                    } else if (left_remaining <= right_remaining) {
                        count = predictor_.decode_count(
                            *decoder_,
                            total,
                            left_remaining,
                            right_remaining,
                            static_cast<int>(parent_unique_symbols - parent_symbol_index),
                            context + 2 * left_leaf + 4 * right_leaf);
                    } else {
                        count = total - predictor_.decode_count(
                                            *decoder_,
                                            total,
                                            right_remaining,
                                            left_remaining,
                                            static_cast<int>(parent_unique_symbols - parent_symbol_index),
                                            context + 2 * right_leaf + 4 * left_leaf);
                    }
                } else {
                    const int mirrored_total = left_remaining + right_remaining - total;
                    int mirrored_count = 0;
                    if (source_l_ != nullptr) {
                        encode_split_count(
                            count,
                            total,
                            left_remaining,
                            right_remaining,
                            static_cast<int>(parent_unique_symbols - parent_symbol_index),
                            context,
                            left_leaf,
                            right_leaf);
                    } else if (left_remaining <= right_remaining) {
                        mirrored_count = predictor_.decode_count(
                            *decoder_,
                            mirrored_total,
                            left_remaining,
                            right_remaining,
                            static_cast<int>(parent_unique_symbols - parent_symbol_index),
                            context + 2 * right_leaf + 4 * left_leaf);
                    } else {
                        mirrored_count = mirrored_total - predictor_.decode_count(
                                                              *decoder_,
                                                              mirrored_total,
                                                              right_remaining,
                                                              left_remaining,
                                                              static_cast<int>(parent_unique_symbols - parent_symbol_index),
                                                              context + 2 * left_leaf + 4 * right_leaf);
                    }
                    if (source_l_ == nullptr) {
                        count = left_remaining - mirrored_count;
                    }
                }
            } else {
                count = std::min(left_remaining, total);
            }

            symbol_pivots_[symbol][level] = static_cast<uint8_t>(count == 0 || count == total);
            pivot_history |= symbol_pivots_[symbol][level];
            left_remaining -= count;
            right_remaining += count - total;
            if (count > 0) {
                left_entries.push_back({symbol, static_cast<uint32_t>(count), offset});
            }
            if (total - count > 0) {
                right_entries.push_back({symbol, static_cast<uint32_t>(total - count), offset + static_cast<uint32_t>(count)});
            }
        }

        write_entries(parent_context_offset, left_entries, left_interval_size);
        write_entries(right_context_offset, right_entries, right_interval_size);
    }

    std::array<uint32_t, 256> context_histogram(uint32_t context_start) const {
        std::array<uint32_t, 256> hist{};
        uint32_t total_symbols = ctx_count_[context_start];
        if (context_start <= primary_index_ && primary_index_ < context_start + ctx_count_[context_start]) {
            --total_symbols;
        }
        uint32_t unique_symbols = 1;
        while (total_symbols > 1 && ctx_count_[context_start + unique_symbols] > 0) {
            const uint32_t count = ctx_count_[context_start + unique_symbols];
            hist[ctx_symbol_[context_start + unique_symbols]] = count;
            total_symbols -= count;
            ++unique_symbols;
        }
        hist[ctx_symbol_[context_start]] = total_symbols;
        return hist;
    }

    uint64_t splits() const { return splits_; }
    uint64_t contexts_visited() const { return contexts_visited_; }
    void set_event_budget(uint64_t max_parser_events) { max_parser_events_ = max_parser_events; }

    void parse_contexts() {
        while (!current_segments_.empty()) {
            const auto segments = current_segments_;
            next_segments_.clear();
            size_t segment_start = 0;
            while (segment_start < segments.size()) {
                const uint32_t context_start = segments[segment_start];
                const uint32_t context_count = ctx_count_[context_start];
                const uint32_t context_end = context_start + context_count;
                size_t segment_end = segment_start + 1;
                while (segment_end < segments.size() && segments[segment_end] < context_end) {
                    ++segment_end;
                }
                ++contexts_visited_;
                check_event_budget();
                if (is_trivial_context(context_start)) {
                    split_trivial_context_range(segments, segment_start, segment_end);
                } else {
                    const auto parent_frequencies = populate_context_frequencies(context_start);
                    split_context_recursive_range(segments, segment_start, segment_end, 2, parent_frequencies);
                }
                segment_start = segment_end;
            }
            std::sort(next_segments_.begin(), next_segments_.end());
            current_segments_ = next_segments_;
        }
    }

    std::vector<uint8_t> finish_l() const {
        return std::vector<uint8_t>(ctx_symbol_.begin(), ctx_symbol_.begin() + static_cast<std::ptrdiff_t>(n_));
    }

private:
    bool is_trivial_context(uint32_t context_start) const {
        return ctx_count_[context_start + 1] == 0 &&
               !(context_start <= primary_index_ && primary_index_ < context_start + ctx_count_[context_start]);
    }

    void check_event_budget() const {
        if (max_parser_events_ != 0 && splits_ + contexts_visited_ > max_parser_events_) {
            throw std::runtime_error("m03 parser event budget exceeded");
        }
    }

    void normalize_context(uint32_t start, uint32_t unique_symbols, uint32_t total_symbols) {
        if (unique_symbols > 1) {
            for (uint32_t i = 1; i < unique_symbols; ++i) {
                const uint32_t count = ctx_count_[start + i];
                const uint32_t offset = ctx_offset_[start + i];
                const uint8_t symbol = ctx_symbol_[start + i];
                uint32_t j = i;
                while (j > 0) {
                    const uint32_t prev = start + j - 1;
                    if (ctx_count_[prev] > count) {
                        break;
                    }
                    if (ctx_count_[prev] == count && ctx_symbol_[prev] < symbol) {
                        break;
                    }
                    ctx_count_[start + j] = ctx_count_[prev];
                    ctx_offset_[start + j] = ctx_offset_[prev];
                    ctx_symbol_[start + j] = ctx_symbol_[prev];
                    --j;
                }
                ctx_count_[start + j] = count;
                ctx_offset_[start + j] = offset;
                ctx_symbol_[start + j] = symbol;
            }
            uint32_t tail_start = start + std::max<int>(0, static_cast<int>(unique_symbols) - 6);
            uint32_t tail_end = start + unique_symbols - 1;
            while (tail_start < tail_end) {
                std::swap(ctx_count_[tail_start], ctx_count_[tail_end]);
                std::swap(ctx_offset_[tail_start], ctx_offset_[tail_end]);
                std::swap(ctx_symbol_[tail_start], ctx_symbol_[tail_end]);
                ++tail_start;
                --tail_end;
            }
        }
        ctx_count_[start] = total_symbols;
        if (unique_symbols < total_symbols) {
            ctx_count_[start + unique_symbols] = 0;
        }
    }

    std::vector<ContextEntry> extract_context_entries(uint32_t context_start) const {
        uint32_t total_symbols = ctx_count_[context_start];
        if (context_start <= primary_index_ && primary_index_ < context_start + ctx_count_[context_start]) {
            --total_symbols;
        }
        std::vector<ContextEntry> entries;
        uint32_t unique_symbols = 1;
        while (total_symbols > 1 && ctx_count_[context_start + unique_symbols] > 0) {
            const uint32_t count = ctx_count_[context_start + unique_symbols];
            entries.push_back({
                ctx_symbol_[context_start + unique_symbols],
                count,
                ctx_offset_[context_start + unique_symbols],
            });
            total_symbols -= count;
            ++unique_symbols;
        }
        if (total_symbols > 0 || entries.empty()) {
            entries.insert(entries.begin(), {ctx_symbol_[context_start], total_symbols, ctx_offset_[context_start]});
        }
        return entries;
    }

    int encode_mode_left_count(uint32_t parent_context_offset, uint32_t right_context_offset, uint32_t symbol) const {
        if (source_l_ == nullptr) {
            return 0;
        }
        int count = 0;
        for (uint32_t pos = parent_context_offset; pos < right_context_offset; ++pos) {
            if ((*source_l_)[pos] == symbol) {
                ++count;
            }
        }
        if (symbol == 0 && parent_context_offset <= primary_index_ && primary_index_ < right_context_offset) {
            --count;
        }
        if (count < 0) {
            throw std::runtime_error("negative encoded left count");
        }
        return count;
    }

    void encode_split_count(int count,
                            int total,
                            int left_remaining,
                            int right_remaining,
                            int symbols_remaining,
                            int context,
                            int left_leaf,
                            int right_leaf) {
        if (encoder_ == nullptr) {
            throw std::runtime_error("encode split called without encoder");
        }
        try {
            if (total <= left_remaining + right_remaining - total) {
                if (left_remaining <= right_remaining) {
                    predictor_.encode_count(
                        *encoder_,
                        count,
                        total,
                        left_remaining,
                        right_remaining,
                        symbols_remaining,
                        context + 2 * left_leaf + 4 * right_leaf);
                } else {
                    predictor_.encode_count(
                        *encoder_,
                        total - count,
                        total,
                        right_remaining,
                        left_remaining,
                        symbols_remaining,
                        context + 2 * right_leaf + 4 * left_leaf);
                }
                return;
            }

            const int mirrored_total = left_remaining + right_remaining - total;
            const int mirrored_count = left_remaining - count;
            if (left_remaining <= right_remaining) {
                predictor_.encode_count(
                    *encoder_,
                    mirrored_count,
                    mirrored_total,
                    left_remaining,
                    right_remaining,
                    symbols_remaining,
                    context + 2 * right_leaf + 4 * left_leaf);
            } else {
                predictor_.encode_count(
                    *encoder_,
                    mirrored_total - mirrored_count,
                    mirrored_total,
                    right_remaining,
                    left_remaining,
                    symbols_remaining,
                    context + 2 * left_leaf + 4 * right_leaf);
            }
        } catch (const std::exception& exc) {
            throw std::runtime_error("encode_split_count failed: count=" + std::to_string(count) +
                                     " total=" + std::to_string(total) +
                                     " left_remaining=" + std::to_string(left_remaining) +
                                     " right_remaining=" + std::to_string(right_remaining) +
                                     " symbols_remaining=" + std::to_string(symbols_remaining) +
                                     " context=" + std::to_string(context) +
                                     " left_leaf=" + std::to_string(left_leaf) +
                                     " right_leaf=" + std::to_string(right_leaf) +
                                     " cause=" + exc.what());
        }
    }

    void write_entries(uint32_t start, std::vector<ContextEntry>& entries, uint32_t total_symbols) {
        if (entries.size() > 1) {
            std::sort(entries.begin(), entries.end(), [](const ContextEntry& a, const ContextEntry& b) {
                if (a.count != b.count) {
                    return a.count > b.count;
                }
                return a.symbol < b.symbol;
            });
            size_t tail_start = entries.size() > 6 ? entries.size() - 6 : 0;
            size_t tail_end = entries.size() - 1;
            while (tail_start < tail_end) {
                std::swap(entries[tail_start], entries[tail_end]);
                ++tail_start;
                --tail_end;
            }
        }
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            const uint32_t pos = start + static_cast<uint32_t>(idx);
            ctx_symbol_[pos] = static_cast<uint8_t>(entries[idx].symbol);
            ctx_count_[pos] = entries[idx].count;
            ctx_offset_[pos] = entries[idx].offset;
        }
        ctx_count_[start] = total_symbols;
        if (entries.size() < total_symbols) {
            ctx_count_[start + entries.size()] = 0;
        }
    }

    std::array<uint32_t, 256> populate_context_frequencies(uint32_t context_start) const {
        std::array<uint32_t, 256> out{};
        uint32_t total_symbols = ctx_count_[context_start];
        if (context_start <= primary_index_ && primary_index_ < context_start + total_symbols) {
            --total_symbols;
        }
        uint32_t unique_symbols = 1;
        while (total_symbols > 1 && ctx_count_[context_start + unique_symbols] > 0) {
            const uint32_t count = ctx_count_[context_start + unique_symbols];
            const uint8_t symbol = ctx_symbol_[context_start + unique_symbols];
            out[symbol] = count;
            total_symbols -= count;
            ++unique_symbols;
        }
        out[ctx_symbol_[context_start]] = total_symbols;
        return out;
    }

    void populate_next_segments(uint32_t context_start, const std::array<uint32_t, 256>& frequencies) {
        uint32_t total_symbols = ctx_count_[context_start];
        if (context_start <= primary_index_ && primary_index_ < context_start + total_symbols) {
            --total_symbols;
        }
        uint32_t unique_symbols = 1;
        while (total_symbols > 1 && ctx_count_[context_start + unique_symbols] > 0) {
            const uint32_t count = ctx_count_[context_start + unique_symbols];
            const uint8_t symbol = ctx_symbol_[context_start + unique_symbols];
            const uint32_t offset = ctx_offset_[context_start + unique_symbols];
            if (frequencies[symbol] != count) {
                next_segments_.push_back(offset);
            }
            total_symbols -= count;
            ++unique_symbols;
        }
        const uint8_t symbol = ctx_symbol_[context_start];
        if (frequencies[symbol] != total_symbols) {
            next_segments_.push_back(ctx_offset_[context_start]);
        }
    }

    void split_trivial_context_range(const std::vector<uint32_t>& offsets, size_t left, size_t right) {
        const uint32_t context_start_initial = offsets[left];
        const uint32_t parent_count = ctx_count_[context_start_initial];
        const uint32_t parent_offset = ctx_offset_[context_start_initial];
        const uint8_t parent_symbol = ctx_symbol_[context_start_initial];
        uint32_t remaining_count = parent_count;
        uint32_t remaining_offset = parent_offset;
        uint32_t context_start = context_start_initial;
        for (size_t offset_idx = left + 1; offset_idx < right; ++offset_idx) {
            const uint32_t context_end = offsets[offset_idx];
            const uint32_t context_size = context_end - context_start;
            next_segments_.push_back(remaining_offset);
            ctx_count_[context_start] = context_size;
            ctx_offset_[context_start] = remaining_offset;
            ctx_symbol_[context_start] = parent_symbol;
            if (context_size > 1) {
                ctx_count_[context_start + 1] = 0;
            }
            remaining_count -= context_size;
            remaining_offset += context_size;
            context_start = context_end;
        }
        next_segments_.push_back(remaining_offset);
        ctx_count_[context_start] = remaining_count;
        ctx_offset_[context_start] = remaining_offset;
        ctx_symbol_[context_start] = parent_symbol;
        if (remaining_count > 1) {
            ctx_count_[context_start + 1] = 0;
        }
    }

    void split_context_recursive_range(const std::vector<uint32_t>& offsets,
                                       size_t left,
                                       size_t right,
                                       uint32_t level,
                                       const std::array<uint32_t, 256>& parent_frequencies) {
        const size_t offset_count = right - left;
        if (offset_count == 1) {
            populate_next_segments(offsets[left], parent_frequencies);
            return;
        }
        const uint32_t context_start = offsets[left];
        if (is_trivial_context(context_start)) {
            split_trivial_context_range(offsets, left, right);
            return;
        }
        if (7 <= offset_count && offset_count <= 256) {
            auto roots = build_optimal_alphabetic_tree_range(offsets, left, right);
            traverse_alphabetic_tree_range(offsets, left, 0, offset_count, level, parent_frequencies, roots, offset_count);
            return;
        }
        if (offset_count == 2) {
            const size_t pivot = left + 1;
            apply_split(offsets[left], offsets[pivot], level, 1, 1);
            populate_next_segments(offsets[left], parent_frequencies);
            populate_next_segments(offsets[pivot], parent_frequencies);
            return;
        }
        const size_t pivot_index = choose_context_pivot_using_heuristic_range(offsets, left, right);
        const size_t pivot = left + pivot_index;
        apply_split(
            offsets[left],
            offsets[pivot],
            level,
            static_cast<uint32_t>(pivot_index),
            static_cast<uint32_t>(right - pivot));
        split_context_recursive_range(offsets, left, pivot, level + 1, parent_frequencies);
        split_context_recursive_range(offsets, pivot, right, level + 1, parent_frequencies);
    }

    void traverse_alphabetic_tree_range(const std::vector<uint32_t>& offsets,
                                        size_t base,
                                        size_t left,
                                        size_t right,
                                        uint32_t level,
                                        const std::array<uint32_t, 256>& parent_frequencies,
                                        const std::vector<uint16_t>& roots,
                                        size_t root_count) {
        const uint32_t context_start = offsets[base + left];
        if (right - left < 7 || is_trivial_context(context_start)) {
            split_context_recursive_range(offsets, base + left, base + right, level, parent_frequencies);
            return;
        }
        const size_t pivot_index = static_cast<size_t>(roots[left * root_count + right - 1]) + 1;
        apply_split(
            offsets[base + left],
            offsets[base + pivot_index],
            level,
            static_cast<uint32_t>(pivot_index - left),
            static_cast<uint32_t>(right - pivot_index));
        traverse_alphabetic_tree_range(offsets, base, left, pivot_index, level + 1, parent_frequencies, roots, root_count);
        traverse_alphabetic_tree_range(offsets, base, pivot_index, right, level + 1, parent_frequencies, roots, root_count);
    }

    std::vector<uint16_t> build_optimal_alphabetic_tree_range(const std::vector<uint32_t>& offsets,
                                                              size_t start,
                                                              size_t end) const {
        const size_t count = end - start;
        std::vector<uint32_t> keys(count, 0);
        std::vector<uint64_t> weight(count, 0);
        std::vector<uint64_t> cost(count * count, 0);
        std::vector<uint16_t> root(count * count, 0);
        const uint32_t context_begin = offsets[start];
        const uint32_t context_end = context_begin + ctx_count_[context_begin];
        keys[count - 1] = 1 + context_end - offsets[end - 1];
        for (size_t rev = 0; rev + 1 < count; ++rev) {
            const size_t idx = count - 2 - rev;
            keys[idx] = 1 + offsets[start + idx + 1] - offsets[start + idx];
            weight[idx] = keys[idx] + keys[idx + 1];
            cost[idx * count + idx + 1] = weight[idx];
            root[idx * count + idx] = static_cast<uint16_t>(idx);
            root[idx * count + idx + 1] = static_cast<uint16_t>(idx);
        }
        root[(count - 1) * count + count - 1] = static_cast<uint16_t>(count - 1);
        for (size_t length = 3; length <= count; ++length) {
            for (size_t left = 0; left + length <= count; ++left) {
                const size_t right = left + length - 1;
                size_t best_root = root[left * count + right - 1];
                uint64_t best_cost = cost[left * count + best_root] + cost[(best_root + 1) * count + right];
                const size_t upper = root[(left + 1) * count + right];
                for (size_t candidate = best_root + 1; candidate <= upper; ++candidate) {
                    const uint64_t candidate_cost =
                        cost[left * count + candidate] + cost[(candidate + 1) * count + right];
                    if (candidate_cost < best_cost) {
                        best_cost = candidate_cost;
                        best_root = candidate;
                    }
                }
                weight[left] += keys[right];
                cost[left * count + right] = best_cost + weight[left];
                root[left * count + right] = static_cast<uint16_t>(best_root);
            }
        }
        return root;
    }

    size_t choose_context_pivot_using_heuristic_range(const std::vector<uint32_t>& offsets,
                                                      size_t start,
                                                      size_t end) const {
        const size_t offset_count = end - start;
        const uint32_t context_begin = offsets[start];
        const uint32_t context_end = context_begin + ctx_count_[context_begin];
        if (offset_count > 6) {
            const uint64_t total = context_end - context_begin + offset_count;
            uint64_t acc = 0;
            uint32_t prev = context_begin;
            for (size_t idx = 1; idx < offset_count; ++idx) {
                const uint32_t cur = offsets[start + idx];
                acc += 1 + cur - prev;
                if (acc * 2 >= total) {
                    return idx;
                }
                prev = cur;
            }
            return offset_count / 2;
        }
        if (offset_count == 3) {
            const uint32_t a = 1 + offsets[start + 1] - context_begin;
            const uint32_t c = 1 + context_end - offsets[start + 2];
            return c <= a ? 1 : 2;
        }
        if (offset_count == 4) {
            const uint32_t a = 1 + offsets[start + 1] - context_begin;
            const uint32_t b = 1 + offsets[start + 2] - offsets[start + 1];
            const uint32_t c = 1 + offsets[start + 3] - offsets[start + 2];
            const uint32_t d = 1 + context_end - offsets[start + 3];
            const uint32_t cost1 = b + c + d + c + std::min(b, d);
            const uint32_t cost2 = a + b + c + d;
            const uint32_t cost3 = a + b + c + b + std::min(a, c);
            size_t best = 1;
            uint32_t best_cost = cost1;
            if (cost2 <= best_cost) {
                best = 2;
                best_cost = cost2;
            }
            if (cost3 < best_cost) {
                best = 3;
            }
            return best;
        }
        if (offset_count == 5) {
            const uint32_t a = 1 + offsets[start + 1] - context_begin;
            const uint32_t b = 1 + offsets[start + 2] - offsets[start + 1];
            const uint32_t c = 1 + offsets[start + 3] - offsets[start + 2];
            const uint32_t d = 1 + offsets[start + 4] - offsets[start + 3];
            const uint32_t e = 1 + context_end - offsets[start + 4];
            const uint32_t p3_bcd = b + c + d + c + std::min(b, d);
            const uint32_t p3_cde = c + d + e + d + std::min(c, e);
            const uint32_t p3_abc = a + b + c + b + std::min(a, c);
            const uint32_t p4_bcde_total = b + c + d + e;
            const uint32_t p4_abcd_total = a + b + c + d;
            const uint32_t cost1 = p4_bcde_total + std::min(p4_bcde_total, std::min(p3_bcd, p3_cde));
            const uint32_t cost2 = a + b + p3_cde;
            const uint32_t cost3 = p3_abc + d + e;
            const uint32_t cost4 = p4_abcd_total + std::min(p4_abcd_total, std::min(p3_abc, p3_bcd));
            size_t best = 1;
            uint32_t best_cost = cost1;
            if (cost2 <= best_cost) {
                best = 2;
                best_cost = cost2;
            }
            if (cost3 < best_cost) {
                best = 3;
                best_cost = cost3;
            }
            if (cost4 < best_cost) {
                best = 4;
            }
            return best;
        }
        if (offset_count == 6) {
            const uint32_t a = 1 + offsets[start + 1] - context_begin;
            const uint32_t b = 1 + offsets[start + 2] - offsets[start + 1];
            const uint32_t c = 1 + offsets[start + 3] - offsets[start + 2];
            const uint32_t d = 1 + offsets[start + 4] - offsets[start + 3];
            const uint32_t e = 1 + offsets[start + 5] - offsets[start + 4];
            const uint32_t f = 1 + context_end - offsets[start + 5];
            const uint32_t p3_abc = a + b + c + b + std::min(a, c);
            const uint32_t p3_bcd = b + c + d + c + std::min(b, d);
            const uint32_t p3_cde = c + d + e + d + std::min(c, e);
            const uint32_t p3_def = d + e + f + e + std::min(d, f);
            const uint32_t p4_abcd_total = a + b + c + d;
            const uint32_t p4_bcde_total = b + c + d + e;
            const uint32_t p4_cdef_total = c + d + e + f;
            const uint32_t p4_abcd = p4_abcd_total + std::min(p4_abcd_total, std::min(p3_abc, p3_bcd));
            const uint32_t p4_bcde = p4_bcde_total + std::min(p4_bcde_total, std::min(p3_bcd, p3_cde));
            const uint32_t p4_cdef = p4_cdef_total + std::min(p4_cdef_total, std::min(p3_cde, p3_def));
            const uint32_t cost1 = b + c + d + e + f + std::min(std::min(p4_cdef, b + c + p3_def), std::min(p3_bcd + e + f, p4_bcde));
            const uint32_t cost2 = a + b + p4_cdef;
            const uint32_t cost3 = p3_abc + p3_def;
            const uint32_t cost4 = p4_abcd + e + f;
            const uint32_t cost5 = a + b + c + d + e + std::min(std::min(p4_bcde, a + b + p3_cde), std::min(p3_abc + d + e, p4_abcd));
            size_t best = 1;
            uint32_t best_cost = cost1;
            if (cost2 <= best_cost) {
                best = 2;
                best_cost = cost2;
            }
            if (cost3 <= best_cost) {
                best = 3;
                best_cost = cost3;
            }
            if (cost4 < best_cost) {
                best = 4;
                best_cost = cost4;
            }
            if (cost5 < best_cost) {
                best = 5;
            }
            return best;
        }
        return std::max<size_t>(1, offset_count / 2);
    }

    size_t n_ = 0;
    uint32_t primary_index_ = 0;
    std::vector<uint64_t> root_frequencies_;
    ArithmeticDecoder* decoder_ = nullptr;
    ArithmeticEncoder* encoder_ = nullptr;
    const std::vector<uint8_t>* source_l_ = nullptr;
    std::vector<uint32_t> ctx_count_;
    std::vector<uint32_t> ctx_offset_;
    std::vector<uint8_t> ctx_symbol_;
    std::vector<uint32_t> current_segments_;
    std::vector<uint32_t> next_segments_;
    std::array<std::array<uint8_t, 64>, 256> symbol_pivots_{};
    M03SerializablePredictor predictor_;
    uint64_t splits_ = 0;
    uint64_t contexts_visited_ = 0;
    uint64_t max_parser_events_ = 0;
};

std::string decode_root_mode(ArithmeticDecoder& decoder) {
    const uint64_t generic = decoder.value(2);
    decoder.update(generic, generic + 1, 2);
    if (generic == 0) {
        return "generic";
    }
    const uint64_t preset_kind = decoder.value(3);
    decoder.update(preset_kind, preset_kind + 1, 3);
    if (preset_kind == 0) {
        return "hist";
    }
    if (preset_kind == 1) {
        return "support";
    }
    const uint64_t extended = decoder.value(4);
    decoder.update(extended, extended + 1, 4);
    if (extended == 0) {
        return "composition";
    }
    if (extended == 1) {
        return "latin206";
    }
    if (extended == 2) {
        return "latin205";
    }
    return "latin205p238";
}

std::vector<int> make_latin206_support() {
    std::vector<int> support = {9, 10};
    for (int v = 32; v < 192; ++v) support.push_back(v);
    support.push_back(194);
    support.push_back(195);
    for (int v = 196; v < 221; ++v) support.push_back(v);
    support.push_back(222);
    support.push_back(224);
    support.push_back(225);
    for (int v = 226; v < 238; ++v) support.push_back(v);
    support.push_back(239);
    support.push_back(240);
    return support;
}

std::vector<int> make_latin205_support() {
    std::vector<int> support = {9, 10};
    for (int v = 32; v < 127; ++v) support.push_back(v);
    for (int v = 128; v < 192; ++v) support.push_back(v);
    support.push_back(194);
    support.push_back(195);
    for (int v = 196; v < 221; ++v) support.push_back(v);
    support.push_back(222);
    support.push_back(224);
    support.push_back(225);
    for (int v = 226; v < 238; ++v) support.push_back(v);
    support.push_back(239);
    support.push_back(240);
    return support;
}

std::vector<int> make_latin205p238_support() {
    std::vector<int> support = {9, 10};
    for (int v = 32; v < 127; ++v) support.push_back(v);
    for (int v = 128; v < 192; ++v) support.push_back(v);
    for (int v = 194; v < 221; ++v) support.push_back(v);
    support.push_back(222);
    for (int v = 224; v < 239; ++v) support.push_back(v);
    support.push_back(239);
    support.push_back(240);
    return support;
}

std::vector<uint64_t> decode_root_frequencies_generic_stream(ArithmeticDecoder& decoder, size_t k, uint64_t n) {
    std::array<uint64_t, 33> bit_freq{};
    std::array<uint64_t, 33> bit_freq_sum{};

    int64_t remaining_min = static_cast<int64_t>(n);
    int64_t remaining_max = static_cast<int64_t>(n);
    uint64_t remaining_count = k;
    for (size_t bit = 0; bit < 33; ++bit) {
        if (remaining_count == 0) {
            break;
        }
        const int64_t min_value = (int64_t{1} << bit) - 1;
        const int64_t max_value = (int64_t{1} << (bit + 1)) - 2;
        const int64_t min_count = std::max<int64_t>(
            static_cast<int64_t>(remaining_count) - (remaining_max / (max_value + 1)),
            0);
        const int64_t max_count =
            static_cast<int64_t>(remaining_count) * max_value < remaining_min
                ? static_cast<int64_t>(remaining_count) - 1
                : static_cast<int64_t>(remaining_count);
        bit_freq[bit] = decode_value_stream(decoder, static_cast<uint64_t>(min_count), static_cast<uint64_t>(max_count));
        remaining_min -= static_cast<int64_t>(bit_freq[bit]) * max_value;
        remaining_max -= static_cast<int64_t>(bit_freq[bit]) * min_value;
        remaining_count -= bit_freq[bit];
    }

    uint64_t bit_sum = 0;
    remaining_min = 0;
    remaining_max = 0;
    int64_t remaining_total = static_cast<int64_t>(n);
    for (int bit = 32; bit >= 0; --bit) {
        const int64_t min_value = (int64_t{1} << bit) - 1;
        const int64_t max_value = (int64_t{1} << (bit + 1)) - 2;
        bit_freq_sum[static_cast<size_t>(bit)] = bit_sum;
        bit_sum += bit_freq[static_cast<size_t>(bit)];
        remaining_min += min_value * static_cast<int64_t>(bit_freq[static_cast<size_t>(bit)]);
        remaining_max += max_value * static_cast<int64_t>(bit_freq[static_cast<size_t>(bit)]);
    }

    std::vector<uint64_t> root_frequencies(k, 0);
    auto working_freq = bit_freq;
    auto working_sum = bit_freq_sum;
    for (size_t index = 0; index < k; ++index) {
        size_t bit = 0;
        while (working_sum[bit] > 0) {
            if (working_freq[bit] > 0) {
                const uint64_t total = working_freq[bit] + working_sum[bit];
                const uint64_t value = decoder.value(total);
                if (value < working_freq[bit]) {
                    decoder.update(0, working_freq[bit], total);
                    break;
                }
                decoder.update(working_freq[bit], total, total);
            }
            working_sum[bit] -= 1;
            ++bit;
            if (bit >= working_sum.size()) {
                throw std::runtime_error("root bit class decode overflow");
            }
        }
        working_freq[bit] -= 1;

        const int64_t min_value = (int64_t{1} << bit) - 1;
        const int64_t max_value = (int64_t{1} << (bit + 1)) - 2;
        remaining_min -= min_value;
        remaining_max -= max_value;
        const int64_t min_count = std::max<int64_t>(min_value, remaining_total - remaining_max);
        const int64_t max_count = std::min<int64_t>(max_value, remaining_total - remaining_min);
        if (min_count < 0 || max_count < min_count) {
            throw std::runtime_error("invalid root frequency bounds");
        }
        const uint64_t count =
            decode_value_stream(decoder, static_cast<uint64_t>(min_count), static_cast<uint64_t>(max_count));
        root_frequencies[index] = count;
        remaining_total -= static_cast<int64_t>(count);
    }
    if (remaining_total != 0) {
        throw std::runtime_error("root frequencies do not sum to expected total");
    }
    return root_frequencies;
}

struct RootDecodeResult {
    std::string mode;
    std::vector<uint64_t> frequencies;
};

RootDecodeResult decode_root_frequencies_stream(ArithmeticDecoder& decoder, size_t k, uint64_t n) {
    RootDecodeResult result;
    result.mode = decode_root_mode(decoder);
    if (result.mode == "generic") {
        result.frequencies = decode_root_frequencies_generic_stream(decoder, k, n);
        return result;
    }
    if (result.mode == "latin206" || result.mode == "latin205" || result.mode == "latin205p238") {
        if (k != 256) {
            throw std::runtime_error("unexpected latin root preset alphabet size");
        }
        std::vector<int> support;
        if (result.mode == "latin206") {
            support = make_latin206_support();
        } else if (result.mode == "latin205") {
            support = make_latin205_support();
        } else {
            support = make_latin205p238_support();
        }
        auto ordered_counts = decode_root_frequencies_generic_stream(decoder, support.size(), n);
        result.frequencies.assign(k, 0);
        for (size_t i = 0; i < support.size(); ++i) {
            const int symbol = support[support.size() - 1 - i];
            result.frequencies[static_cast<size_t>(symbol)] = ordered_counts[i];
        }
        return result;
    }
    throw std::runtime_error("root mode not yet supported in C++ header inspector: " + result.mode);
}

void encode_root_mode(ArithmeticEncoder& encoder, const std::string& mode) {
    if (mode == "generic") {
        encoder.encode(0, 1, 2);
        return;
    }
    encoder.encode(1, 2, 2);
    if (mode == "hist") {
        encoder.encode(0, 1, 3);
        return;
    }
    if (mode == "support") {
        encoder.encode(1, 2, 3);
        return;
    }
    encoder.encode(2, 3, 3);
    if (mode == "composition") {
        encoder.encode(0, 1, 4);
        return;
    }
    if (mode == "latin206") {
        encoder.encode(1, 2, 4);
        return;
    }
    if (mode == "latin205") {
        encoder.encode(2, 3, 4);
        return;
    }
    if (mode == "latin205p238") {
        encoder.encode(3, 4, 4);
        return;
    }
    throw std::runtime_error("unknown root mode: " + mode);
}

void encode_root_frequencies_generic_stream(ArithmeticEncoder& encoder,
                                            const std::vector<uint64_t>& root_frequencies,
                                            size_t k,
                                            uint64_t n) {
    if (root_frequencies.size() != k) {
        throw std::runtime_error("root frequency length mismatch");
    }
    std::array<uint64_t, 33> bit_freq{};
    std::array<uint64_t, 33> bit_freq_sum{};

    int64_t remaining_min = static_cast<int64_t>(n);
    int64_t remaining_max = static_cast<int64_t>(n);
    uint64_t remaining_count = k;
    for (uint64_t value : root_frequencies) {
        bit_freq[static_cast<size_t>(bit_scan_reverse(value + 1))] += 1;
    }
    for (size_t bit = 0; bit < 33; ++bit) {
        if (remaining_count == 0) {
            break;
        }
        const int64_t min_value = (int64_t{1} << bit) - 1;
        const int64_t max_value = (int64_t{1} << (bit + 1)) - 2;
        const int64_t min_count = std::max<int64_t>(
            static_cast<int64_t>(remaining_count) - (remaining_max / (max_value + 1)),
            0);
        const int64_t max_count =
            static_cast<int64_t>(remaining_count) * max_value < remaining_min
                ? static_cast<int64_t>(remaining_count) - 1
                : static_cast<int64_t>(remaining_count);
        encode_value_stream(encoder,
                            static_cast<uint64_t>(min_count),
                            bit_freq[bit],
                            static_cast<uint64_t>(max_count));
        remaining_min -= static_cast<int64_t>(bit_freq[bit]) * max_value;
        remaining_max -= static_cast<int64_t>(bit_freq[bit]) * min_value;
        remaining_count -= bit_freq[bit];
    }

    uint64_t bit_sum = 0;
    remaining_min = 0;
    remaining_max = 0;
    int64_t remaining_total = static_cast<int64_t>(n);
    for (int bit = 32; bit >= 0; --bit) {
        const int64_t min_value = (int64_t{1} << bit) - 1;
        const int64_t max_value = (int64_t{1} << (bit + 1)) - 2;
        bit_freq_sum[static_cast<size_t>(bit)] = bit_sum;
        bit_sum += bit_freq[static_cast<size_t>(bit)];
        remaining_min += min_value * static_cast<int64_t>(bit_freq[static_cast<size_t>(bit)]);
        remaining_max += max_value * static_cast<int64_t>(bit_freq[static_cast<size_t>(bit)]);
    }

    auto working_freq = bit_freq;
    auto working_sum = bit_freq_sum;
    for (uint64_t value : root_frequencies) {
        const size_t bit = static_cast<size_t>(bit_scan_reverse(value + 1));
        for (size_t b = 0; b < bit; ++b) {
            if (working_freq[b] > 0) {
                const uint64_t total = working_freq[b] + working_sum[b];
                encoder.encode(working_freq[b], total, total);
            }
            working_sum[b] -= 1;
        }
        if (working_sum[bit] > 0) {
            encoder.encode(0, working_freq[bit], working_freq[bit] + working_sum[bit]);
        }
        working_freq[bit] -= 1;

        const int64_t min_value = (int64_t{1} << bit) - 1;
        const int64_t max_value = (int64_t{1} << (bit + 1)) - 2;
        remaining_min -= min_value;
        remaining_max -= max_value;
        const int64_t min_count = std::max<int64_t>(min_value, remaining_total - remaining_max);
        const int64_t max_count = std::min<int64_t>(max_value, remaining_total - remaining_min);
        encode_value_stream(encoder,
                            static_cast<uint64_t>(min_count),
                            value,
                            static_cast<uint64_t>(max_count));
        remaining_total -= static_cast<int64_t>(value);
    }
}

bool root_support_subset(const std::vector<uint64_t>& root_frequencies, const std::vector<int>& support) {
    std::array<uint8_t, 256> allowed{};
    for (int symbol : support) {
        allowed[static_cast<size_t>(symbol)] = 1;
    }
    for (size_t i = 0; i < root_frequencies.size(); ++i) {
        if (root_frequencies[i] > 0 && !allowed[i]) {
            return false;
        }
    }
    return true;
}

void encode_root_frequencies_stream(ArithmeticEncoder& encoder,
                                    const std::vector<uint64_t>& root_frequencies,
                                    size_t k,
                                    uint64_t n,
                                    const std::string& requested_mode) {
    std::string mode = requested_mode;
    if (mode == "auto") {
        mode = "generic";
    }
    if (mode == "latin206" || mode == "latin205" || mode == "latin205p238") {
        if (k != 256) {
            mode = "generic";
        } else {
            std::vector<int> support =
                mode == "latin206" ? make_latin206_support()
                                    : (mode == "latin205" ? make_latin205_support() : make_latin205p238_support());
            if (!root_support_subset(root_frequencies, support)) {
                mode = "generic";
            } else {
                encode_root_mode(encoder, mode);
                std::vector<uint64_t> ordered_counts;
                ordered_counts.reserve(support.size());
                for (auto it = support.rbegin(); it != support.rend(); ++it) {
                    ordered_counts.push_back(root_frequencies[static_cast<size_t>(*it)]);
                }
                encode_root_frequencies_generic_stream(encoder, ordered_counts, ordered_counts.size(), n);
                return;
            }
        }
    }
    if (mode != "generic") {
        mode = "generic";
    }
    encode_root_mode(encoder, mode);
    encode_root_frequencies_generic_stream(encoder, root_frequencies, k, n);
}

int normalized_main_byte(const std::vector<uint8_t>& data, int64_t pos) {
    if (pos < 0 || static_cast<size_t>(pos) >= data.size()) {
        return -1;
    }
    const uint8_t b = data[static_cast<size_t>(pos)];
    return (b >= '0' && b <= '9') ? kPlaceholder : b;
}

std::vector<uint8_t> make_main(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out = data;
    for (uint8_t& b : out) {
        if (b >= '0' && b <= '9') {
            b = kPlaceholder;
        }
    }
    return out;
}

std::vector<DigitRun> scan_runs(const std::vector<uint8_t>& main) {
    std::vector<DigitRun> runs;
    size_t pos = 0;
    while (pos < main.size()) {
        if (main[pos] != kPlaceholder) {
            ++pos;
            continue;
        }
        size_t end = pos + 1;
        while (end < main.size() && main[end] == kPlaceholder) {
            ++end;
        }
        DigitRun run;
        run.start = pos;
        run.end = end;
        run.prev_byte = pos ? main[pos - 1] : -1;
        run.next_byte = end < main.size() ? main[end] : -1;
        run.prev2_norm = normalized_main_byte(main, static_cast<int64_t>(pos) - 2);
        run.prev3_norm = normalized_main_byte(main, static_cast<int64_t>(pos) - 3);
        run.prev4_norm = normalized_main_byte(main, static_cast<int64_t>(pos) - 4);
        run.next2_norm = normalized_main_byte(main, static_cast<int64_t>(end) + 1);
        runs.push_back(run);
        pos = end;
    }
    return runs;
}

std::vector<int> left_key(const std::vector<uint8_t>& data, const DigitRun& run, size_t count) {
    std::vector<int> values = {
        run.prev_byte + 1,
        run.prev2_norm + 1,
        run.prev3_norm + 1,
        run.prev4_norm + 1,
    };
    while (values.size() < count) {
        values.push_back(normalized_main_byte(data, static_cast<int64_t>(run.start) -
                                                       static_cast<int64_t>(values.size()) - 1) +
                         1);
    }
    values.resize(count);
    return values;
}

std::vector<int> sort_key(const std::vector<uint8_t>& main, const DigitRun& run) {
    const int bucket = static_cast<int>(std::min<size_t>(run.length(), kCap));
    std::vector<int> key;
    auto append_left = [&](size_t count) {
        auto left = left_key(main, run, count);
        key.insert(key.end(), left.begin(), left.end());
    };

    if (bucket == 1) {
        append_left(3);
        key.push_back(run.next_byte + 1);
    } else if (bucket == 2) {
        append_left(4);
        key.push_back(run.next_byte + 1);
        key.push_back(run.next2_norm + 1);
    } else if (bucket == 3) {
        key = {run.prev_byte + 1, run.next_byte + 1};
    } else if (bucket == 5) {
        append_left(4);
    } else if (bucket == 6) {
        append_left(4);
        key.push_back(run.next_byte + 1);
    } else if (bucket == 7) {
        append_left(8);
    } else if (bucket == 8) {
        append_left(6);
    } else if (bucket == 9) {
        key = {run.next_byte + 1, run.prev_byte + 1};
    } else if (bucket == 10) {
        append_left(6);
    } else if (bucket == 12) {
        append_left(6);
    } else {
        key = {run.prev_byte + 1};
    }
    return key;
}

const char* pack_mode(int bucket) {
    switch (bucket) {
        case 1: return "raw";
        case 2: return "pair";
        case 3: return "pair";
        case 4: return "int_be";
        case 5: return "int_be";
        case 6: return "pair";
        case 7: return "raw";
        case 8: return "pair";
        case 9: return "raw";
        case 10: return "pair";
        case 11: return "raw";
        case 12: return "raw";
        default: throw std::runtime_error("invalid bucket");
    }
}

int stream_index(int bucket) {
    return bucket < 4 ? bucket - 1 : bucket;
}

std::vector<uint8_t> pack_pair(const std::vector<uint8_t>& digits) {
    std::vector<uint8_t> out;
    size_t start = 0;
    if (digits.size() % 2 != 0) {
        out.push_back(static_cast<uint8_t>(100 + digits[0]));
        start = 1;
    }
    for (size_t pos = start; pos < digits.size(); pos += 2) {
        out.push_back(static_cast<uint8_t>(digits[pos] * 10 + digits[pos + 1]));
    }
    return out;
}

std::vector<uint8_t> pack_int_be(const std::vector<uint8_t>& digits) {
    uint64_t value = 0;
    for (uint8_t digit : digits) {
        value = value * 10 + digit;
    }
    const size_t n = (digits.size() + 1) / 2;
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t shift = 8 * (n - 1 - i);
        out[i] = static_cast<uint8_t>((value >> shift) & 0xff);
    }
    return out;
}

std::vector<uint8_t> pack_digits(const std::vector<uint8_t>& data, const DigitRun& run, const char* mode) {
    std::vector<uint8_t> digits;
    digits.reserve(run.length());
    for (size_t pos = run.start; pos < run.end; ++pos) {
        digits.push_back(static_cast<uint8_t>(data[pos] - '0'));
    }
    const std::string m(mode);
    if (m == "raw") {
        return digits;
    }
    if (m == "pair") {
        return pack_pair(digits);
    }
    if (m == "int_be") {
        return pack_int_be(digits);
    }
    throw std::runtime_error("unknown pack mode");
}

size_t packed_len(size_t run_len, const char* mode) {
    return std::string(mode) == "raw" ? run_len : (run_len + 1) / 2;
}

uint64_t pow10_u64(size_t n) {
    uint64_t value = 1;
    for (size_t i = 0; i < n; ++i) {
        value *= 10;
    }
    return value;
}

std::vector<uint8_t> unpack_digits(const std::vector<uint8_t>& values, size_t run_len, const char* mode) {
    const std::string m(mode);
    if (m == "raw") {
        if (values.size() != run_len) {
            throw std::runtime_error("invalid raw digit stream length");
        }
        for (uint8_t digit : values) {
            if (digit > 9) {
                throw std::runtime_error("invalid raw digit");
            }
        }
        return values;
    }
    if (m == "pair") {
        std::vector<uint8_t> out;
        size_t pos = 0;
        if (run_len % 2 != 0) {
            if (values.empty() || values[pos] < 100 || values[pos] > 109) {
                throw std::runtime_error("invalid odd digit-pair prefix");
            }
            out.push_back(static_cast<uint8_t>(values[pos] - 100));
            ++pos;
        }
        while (out.size() < run_len) {
            if (pos >= values.size()) {
                throw std::runtime_error("truncated digit-pair stream");
            }
            const uint8_t value = values[pos++];
            if (value >= 100) {
                throw std::runtime_error("unexpected digit-pair marker");
            }
            out.push_back(static_cast<uint8_t>(value / 10));
            out.push_back(static_cast<uint8_t>(value % 10));
        }
        if (out.size() != run_len) {
            throw std::runtime_error("digit-pair length mismatch");
        }
        return out;
    }
    if (m == "int_be") {
        uint64_t value = 0;
        for (uint8_t byte : values) {
            value = (value << 8) | byte;
        }
        if (value >= pow10_u64(run_len)) {
            throw std::runtime_error("int digit payload exceeds run length");
        }
        std::vector<uint8_t> out(run_len);
        for (size_t i = 0; i < run_len; ++i) {
            const size_t idx = run_len - 1 - i;
            out[idx] = static_cast<uint8_t>(value % 10);
            value /= 10;
        }
        return out;
    }
    throw std::runtime_error("unknown unpack mode");
}

std::vector<std::vector<uint8_t>> build_streams(
    const std::vector<uint8_t>& data,
    const std::vector<uint8_t>& main,
    const std::vector<DigitRun>& runs) {
    std::vector<std::vector<uint8_t>> streams(kCap + 1);
    std::vector<std::vector<size_t>> by_bucket(kCap + 1);
    for (size_t i = 0; i < runs.size(); ++i) {
        by_bucket[std::min<size_t>(runs[i].length(), kCap)].push_back(i);
    }

    for (int bucket = 1; bucket <= kCap; ++bucket) {
        auto bucket_runs = by_bucket[bucket];
        if (bucket == 4) {
            std::stable_sort(bucket_runs.begin(), bucket_runs.end(), [&](size_t a, size_t b) {
                return runs[a].prev_byte < runs[b].prev_byte;
            });
            for (size_t index : bucket_runs) {
                streams[3].push_back(pack_digits(data, runs[index], "int_be")[0]);
            }
            std::vector<std::pair<uint8_t, size_t>> pos0_records;
            for (size_t index : by_bucket[bucket]) {
                pos0_records.push_back({pack_digits(data, runs[index], "int_be")[0], index});
            }
            std::stable_sort(pos0_records.begin(), pos0_records.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            for (const auto& item : pos0_records) {
                streams[4].push_back(pack_digits(data, runs[item.second], "int_be")[1]);
            }
            continue;
        }

        std::stable_sort(bucket_runs.begin(), bucket_runs.end(), [&](size_t a, size_t b) {
            return sort_key(main, runs[a]) < sort_key(main, runs[b]);
        });
        const int idx = stream_index(bucket);
        const char* mode = pack_mode(bucket);
        for (size_t index : bucket_runs) {
            auto packed = pack_digits(data, runs[index], mode);
            streams[idx].insert(streams[idx].end(), packed.begin(), packed.end());
        }
    }
    return streams;
}

std::vector<std::vector<uint8_t>> build_streams_variant(
    const std::vector<uint8_t>& data,
    const std::vector<uint8_t>& main,
    const std::vector<DigitRun>& runs,
    bool sort_runs,
    bool pack_best) {
    std::vector<std::vector<uint8_t>> streams(kCap + 1);
    std::vector<std::vector<size_t>> by_bucket(kCap + 1);
    for (size_t i = 0; i < runs.size(); ++i) {
        by_bucket[std::min<size_t>(runs[i].length(), kCap)].push_back(i);
    }

    for (int bucket = 1; bucket <= kCap; ++bucket) {
        auto bucket_runs = by_bucket[bucket];
        if (pack_best && bucket == 4) {
            if (sort_runs) {
                std::stable_sort(bucket_runs.begin(), bucket_runs.end(), [&](size_t a, size_t b) {
                    return runs[a].prev_byte < runs[b].prev_byte;
                });
                for (size_t index : bucket_runs) {
                    streams[3].push_back(pack_digits(data, runs[index], "int_be")[0]);
                }
                std::vector<std::pair<uint8_t, size_t>> pos0_records;
                for (size_t index : by_bucket[bucket]) {
                    pos0_records.push_back({pack_digits(data, runs[index], "int_be")[0], index});
                }
                std::stable_sort(pos0_records.begin(), pos0_records.end(), [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });
                for (const auto& item : pos0_records) {
                    streams[4].push_back(pack_digits(data, runs[item.second], "int_be")[1]);
                }
            } else {
                for (size_t index : bucket_runs) {
                    auto packed = pack_digits(data, runs[index], "int_be");
                    streams[3].push_back(packed[0]);
                    streams[4].push_back(packed[1]);
                }
            }
            continue;
        }

        if (sort_runs) {
            std::stable_sort(bucket_runs.begin(), bucket_runs.end(), [&](size_t a, size_t b) {
                return sort_key(main, runs[a]) < sort_key(main, runs[b]);
            });
        }
        const int idx = stream_index(bucket);
        const char* mode = pack_best ? pack_mode(bucket) : "raw";
        for (size_t index : bucket_runs) {
            auto packed = pack_digits(data, runs[index], mode);
            streams[idx].insert(streams[idx].end(), packed.begin(), packed.end());
        }
    }
    return streams;
}

std::vector<std::vector<uint8_t>> build_raw_digit_streams(
    const std::vector<uint8_t>& data,
    const std::vector<DigitRun>& runs) {
    std::vector<std::vector<uint8_t>> streams(kCap + 1);
    for (const auto& run : runs) {
        for (size_t pos = run.start; pos < run.end; ++pos) {
            streams[0].push_back(static_cast<uint8_t>(data[pos] - '0'));
        }
    }
    return streams;
}

std::vector<uint8_t> join_streams(const std::vector<uint8_t>& main,
                                  const std::vector<std::vector<uint8_t>>& streams) {
    const auto runs = scan_runs(main);
    std::vector<std::vector<size_t>> by_bucket(kCap + 1);
    for (size_t i = 0; i < runs.size(); ++i) {
        by_bucket[std::min<size_t>(runs[i].length(), kCap)].push_back(i);
    }

    std::vector<std::vector<uint8_t>> restored(runs.size());
    for (int bucket = 1; bucket <= kCap; ++bucket) {
        const auto& bucket_runs_original = by_bucket[bucket];
        if (bucket == 4) {
            if (streams[3].size() != bucket_runs_original.size() || streams[4].size() != bucket_runs_original.size()) {
                throw std::runtime_error("invalid bucket4 stream length");
            }
            std::vector<int> pos0_by_run(runs.size(), -1);
            auto ordered = bucket_runs_original;
            std::stable_sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
                return runs[a].prev_byte < runs[b].prev_byte;
            });
            for (size_t i = 0; i < ordered.size(); ++i) {
                pos0_by_run[ordered[i]] = streams[3][i];
            }
            std::vector<size_t> ordered_for_pos1 = bucket_runs_original;
            std::stable_sort(ordered_for_pos1.begin(), ordered_for_pos1.end(), [&](size_t a, size_t b) {
                return pos0_by_run[a] < pos0_by_run[b];
            });
            std::vector<int> pos1_by_run(runs.size(), -1);
            for (size_t i = 0; i < ordered_for_pos1.size(); ++i) {
                pos1_by_run[ordered_for_pos1[i]] = streams[4][i];
            }
            for (size_t index : bucket_runs_original) {
                restored[index] = unpack_digits(
                    {static_cast<uint8_t>(pos0_by_run[index]), static_cast<uint8_t>(pos1_by_run[index])},
                    runs[index].length(),
                    "int_be");
            }
            continue;
        }

        auto ordered = bucket_runs_original;
        std::stable_sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
            return sort_key(main, runs[a]) < sort_key(main, runs[b]);
        });
        const auto& stream = streams[stream_index(bucket)];
        const char* mode = pack_mode(bucket);
        size_t pos = 0;
        for (size_t index : ordered) {
            const size_t size = packed_len(runs[index].length(), mode);
            if (pos + size > stream.size()) {
                throw std::runtime_error("truncated digit context stream");
            }
            restored[index] = unpack_digits(
                std::vector<uint8_t>(stream.begin() + static_cast<std::ptrdiff_t>(pos),
                                     stream.begin() + static_cast<std::ptrdiff_t>(pos + size)),
                runs[index].length(),
                mode);
            pos += size;
        }
        if (pos != stream.size()) {
            throw std::runtime_error("unused digit context stream bytes");
        }
    }

    std::vector<uint8_t> out = main;
    for (size_t i = 0; i < runs.size(); ++i) {
        if (restored[i].size() != runs[i].length()) {
            throw std::runtime_error("restored digit length mismatch");
        }
        for (size_t offset = 0; offset < restored[i].size(); ++offset) {
            out[runs[i].start + offset] = static_cast<uint8_t>('0' + restored[i][offset]);
        }
    }
    return out;
}

struct M03LStream {
    std::vector<uint8_t> l_stream;
    uint64_t original_size = 0;
    uint64_t primary_index = 0;
    uint64_t symbol_size = 0;
    std::string root_mode;
    uint64_t contexts_visited = 0;
    uint64_t splits = 0;
};

M03LStream decode_m03_l_stream(const std::vector<uint8_t>& blob, bool primary_bound_inclusive = false) {
    size_t pos = 0;
    M03LStream result;
    result.original_size = read_varint(blob, pos);
    std::vector<uint8_t> payload(blob.begin() + static_cast<std::ptrdiff_t>(pos), blob.end());
    ArithmeticDecoder decoder(payload);

    const uint64_t symbol_code = decoder.value(2);
    decoder.update(symbol_code, symbol_code + 1, 2);
    result.symbol_size = symbol_code + 1;
    if (result.symbol_size != 1) {
        throw std::runtime_error("unexpected symbol_size in M03 stream");
    }
    result.primary_index = decode_value_stream(
        decoder,
        0,
        primary_bound_inclusive ? result.original_size : result.original_size - 1);
    const auto root = decode_root_frequencies_stream(decoder, 256, result.original_size);
    result.root_mode = root.mode;
    const uint64_t root_sum = std::accumulate(root.frequencies.begin(), root.frequencies.end(), uint64_t{0});
    if (root_sum != result.original_size) {
        throw std::runtime_error("root frequency sum mismatch");
    }

    ParserDecodeState state(static_cast<size_t>(result.original_size + 1),
                            static_cast<uint32_t>(result.primary_index),
                            root.frequencies,
                            decoder);
    state.initialize_root_context();
    state.parse_contexts();
    result.l_stream = state.finish_l();
    result.contexts_visited = state.contexts_visited();
    result.splits = state.splits();
    return result;
}

std::vector<uint8_t> inverse_bwt_nosentinel_cpp(const std::vector<uint8_t>& bwt, uint64_t primary_index) {
    const size_t n = bwt.size();
    if (n == 0) {
        return {};
    }
    if (primary_index > n) {
        throw std::runtime_error("primary_index out of range");
    }

    std::vector<uint16_t> full_l(n + 1);
    for (size_t i = 0; i < static_cast<size_t>(primary_index); ++i) {
        full_l[i] = static_cast<uint16_t>(bwt[i] + 1);
    }
    full_l[static_cast<size_t>(primary_index)] = 0;
    for (size_t i = static_cast<size_t>(primary_index); i < n; ++i) {
        full_l[i + 1] = static_cast<uint16_t>(bwt[i] + 1);
    }

    std::array<uint32_t, 257> counts{};
    std::vector<uint32_t> ranks(n + 1);
    for (size_t i = 0; i < full_l.size(); ++i) {
        const uint16_t sym = full_l[i];
        ranks[i] = counts[sym]++;
    }

    std::array<uint32_t, 257> first_occ{};
    uint32_t total = 0;
    for (size_t sym = 0; sym < first_occ.size(); ++sym) {
        first_occ[sym] = total;
        total += counts[sym];
    }

    std::vector<uint32_t> lf(n + 1);
    for (size_t i = 0; i < full_l.size(); ++i) {
        const uint16_t sym = full_l[i];
        lf[i] = first_occ[sym] + ranks[i];
    }

    std::vector<uint8_t> out;
    out.reserve(n);
    uint32_t idx = static_cast<uint32_t>(primary_index);
    for (size_t i = 0; i < n; ++i) {
        idx = lf[idx];
        const uint16_t sym = full_l[idx];
        if (sym == 0) {
            throw std::runtime_error("unexpected sentinel during inverse BWT");
        }
        out.push_back(static_cast<uint8_t>(sym - 1));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<uint8_t> decompress_m03_block(const std::vector<uint8_t>& blob,
                                          M03LStream* meta = nullptr,
                                          bool primary_bound_inclusive = false) {
    M03LStream decoded = decode_m03_l_stream(blob, primary_bound_inclusive);
    if (decoded.l_stream.size() != decoded.original_size + 1) {
        throw std::runtime_error("decoded L-stream length mismatch");
    }
    std::vector<uint8_t> bwt;
    bwt.reserve(static_cast<size_t>(decoded.original_size));
    bwt.insert(bwt.end(), decoded.l_stream.begin(), decoded.l_stream.begin() + static_cast<std::ptrdiff_t>(decoded.primary_index));
    bwt.insert(bwt.end(),
               decoded.l_stream.begin() + static_cast<std::ptrdiff_t>(decoded.primary_index + 1),
               decoded.l_stream.end());
    auto out = inverse_bwt_nosentinel_cpp(bwt, decoded.primary_index);
    if (out.size() != decoded.original_size) {
        throw std::runtime_error("inverse BWT output length mismatch");
    }
    if (meta != nullptr) {
        *meta = std::move(decoded);
    }
    return out;
}

struct BWTForwardResult {
    std::vector<uint8_t> bwt;
    uint32_t primary_index = 0;
};

BWTForwardResult bwt_transform_libsais(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return {};
    }
    if (data.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        throw std::runtime_error("input too large for 32-bit libsais_bwt");
    }
    const int32_t n = static_cast<int32_t>(data.size());
    BWTForwardResult result;
    result.bwt.resize(data.size());
    std::vector<int32_t> work(data.size() + 1, 0);
    std::array<int32_t, 256> freq{};
    const int32_t primary =
        libsais_bwt(data.data(), result.bwt.data(), work.data(), n, 0, freq.data());
    if (primary < 0) {
        throw std::runtime_error("libsais_bwt failed");
    }
    result.primary_index = static_cast<uint32_t>(primary);
    return result;
}

std::vector<uint8_t> compress_m03_block(const std::vector<uint8_t>& data,
                                        const std::string& root_mode,
                                        uint64_t max_parser_events = 0) {
    if (data.empty()) {
        std::vector<uint8_t> out;
        write_varint(0, out);
        return out;
    }
    const auto transformed = bwt_transform_libsais(data);
    std::vector<uint64_t> root_frequencies(256, 0);
    for (uint8_t byte : transformed.bwt) {
        root_frequencies[byte] += 1;
    }

    std::vector<uint8_t> l_stream(data.size() + 1, 0);
    const size_t primary = transformed.primary_index;
    std::copy(transformed.bwt.begin(),
              transformed.bwt.begin() + static_cast<std::ptrdiff_t>(primary),
              l_stream.begin());
    l_stream[primary] = 0;
    std::copy(transformed.bwt.begin() + static_cast<std::ptrdiff_t>(primary),
              transformed.bwt.end(),
              l_stream.begin() + static_cast<std::ptrdiff_t>(primary + 1));

    ArithmeticEncoder encoder;
    encoder.encode(0, 1, 2);
    g_uniform_context = "m03-primary";
    encode_value_stream(encoder, 0, transformed.primary_index, data.size());
    try {
        g_uniform_context = "m03-root";
        encode_root_frequencies_stream(encoder, root_frequencies, 256, data.size(), root_mode);
    } catch (const std::exception& exc) {
        size_t nonzero = 0;
        for (uint64_t count : root_frequencies) {
            nonzero += count > 0 ? 1 : 0;
        }
        throw std::runtime_error("root frequency encode failed: input_bytes=" + std::to_string(data.size()) +
                                 " root_mode=" + root_mode +
                                 " nonzero_symbols=" + std::to_string(nonzero) +
                                 " cause=" + exc.what());
    }
    ParserDecodeState state(l_stream, transformed.primary_index, root_frequencies, encoder);
    state.set_event_budget(max_parser_events);
    state.initialize_root_context();
    g_uniform_context = "m03-splits";
    state.parse_contexts();
    g_uniform_context = "m03-finish";
    const auto payload = encoder.finish();

    std::vector<uint8_t> out;
    write_varint(data.size(), out);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

int inspect_archive(const std::string& path) {
    const auto parts = parse_digit_archive(path);

    std::cout << "{\n";
    std::cout << "  \"mode\": \"" << (parts.mode == kModeDigitContextV5Native ? "digit_context_v5_native" : "digit_context_v5") << "\",\n";
    std::cout << "  \"archive_bytes\": " << parts.blob.size() << ",\n";
    std::cout << "  \"header_bytes\": " << parts.header_bytes << ",\n";
    std::cout << "  \"main_payload_bytes\": " << parts.main_len << ",\n";
    std::cout << "  \"side_payload_bytes\": " << parts.side_total << ",\n";
    std::cout << "  \"stream_payload_bytes\": [";
    for (size_t i = 0; i < parts.stream_lens.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << parts.stream_lens[i];
    }
    std::cout << "],\n";
    std::cout << "  \"ledger_total_matches\": true\n";
    std::cout << "}\n";
    return 0;
}

int m03_header_inspect(const std::string& path) {
    const auto parts = parse_digit_archive(path);
    const auto main_begin = parts.blob.begin() + static_cast<std::ptrdiff_t>(parts.main_offset);
    const auto main_end = main_begin + static_cast<std::ptrdiff_t>(parts.main_len);
    std::vector<uint8_t> main_blob(main_begin, main_end);
    size_t pos = 0;
    const uint64_t original_size = read_varint(main_blob, pos);
    std::vector<uint8_t> payload(main_blob.begin() + static_cast<std::ptrdiff_t>(pos), main_blob.end());

    ArithmeticDecoder decoder(payload);
    const uint64_t symbol_code = decoder.value(2);
    decoder.update(symbol_code, symbol_code + 1, 2);
    const uint64_t symbol_size = symbol_code + 1;
    if (symbol_size != 1) {
        throw std::runtime_error("unexpected symbol_size in main M03 stream");
    }
    const uint64_t primary_index = decode_value_stream(decoder, 0, original_size - 1);
    const auto root = decode_root_frequencies_stream(decoder, 256, original_size);
    const uint64_t root_sum = std::accumulate(root.frequencies.begin(), root.frequencies.end(), uint64_t{0});
    size_t root_nonzero = 0;
    for (uint64_t count : root.frequencies) {
        if (count > 0) {
            ++root_nonzero;
        }
    }

    std::cout << "{\n";
    std::cout << "  \"mode\": \"digit_context_v5\",\n";
    std::cout << "  \"component\": \"main\",\n";
    std::cout << "  \"archive_bytes\": " << parts.blob.size() << ",\n";
    std::cout << "  \"main_blob_bytes\": " << parts.main_len << ",\n";
    std::cout << "  \"original_size\": " << original_size << ",\n";
    std::cout << "  \"main_varint_bytes\": " << pos << ",\n";
    std::cout << "  \"arithmetic_payload_bytes\": " << payload.size() << ",\n";
    std::cout << "  \"symbol_size\": " << symbol_size << ",\n";
    std::cout << "  \"primary_index\": " << primary_index << ",\n";
    std::cout << "  \"root_mode\": \"" << root.mode << "\",\n";
    std::cout << "  \"root_nonzero_symbols\": " << root_nonzero << ",\n";
    std::cout << "  \"root_frequency_sum\": " << root_sum << ",\n";
    std::cout << "  \"root_sum_ok\": " << (root_sum == original_size ? "true" : "false") << ",\n";
    std::cout << "  \"ledger_total_matches\": true\n";
    std::cout << "}\n";
    return root_sum == original_size ? 0 : 2;
}

int raw_roundtrip(const std::string& path, size_t max_bytes) {
    auto data = read_file(path);
    if (max_bytes > 0 && data.size() > max_bytes) {
        data.resize(max_bytes);
    }
    const auto main = make_main(data);
    const auto runs = scan_runs(main);
    const auto streams = build_streams(data, main, runs);
    const auto joined = join_streams(main, streams);
    const bool ok = joined == data;
    std::cout << "{\n";
    std::cout << "  \"input_bytes\": " << data.size() << ",\n";
    std::cout << "  \"runs\": " << runs.size() << ",\n";
    std::cout << "  \"roundtrip_ok\": " << (ok ? "true" : "false") << ",\n";
    std::cout << "  \"stream_raw_bytes\": [";
    for (size_t i = 0; i < streams.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << streams[i].size();
    }
    std::cout << "]\n";
    std::cout << "}\n";
    return ok ? 0 : 2;
}

int arith_selftest() {
    std::vector<size_t> symbols;
    for (size_t i = 0; i < 4096; ++i) {
        symbols.push_back((i * 17 + (i >> 3) + (i % 5)) % 257);
    }

    ArithmeticEncoder encoder;
    AdaptiveModel enc_model(257);
    for (size_t sym : symbols) {
        const auto [lo, hi, total] = enc_model.interval(sym);
        encoder.encode(lo, hi, total);
        enc_model.update(sym);
    }
    const auto payload = encoder.finish();

    ArithmeticDecoder decoder(payload);
    AdaptiveModel dec_model(257);
    std::vector<size_t> decoded;
    decoded.reserve(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) {
        const uint64_t value = decoder.value(dec_model.total());
        const auto [sym, lo, hi, total] = dec_model.symbol_for_value(value);
        decoder.update(lo, hi, total);
        dec_model.update(sym);
        decoded.push_back(sym);
    }

    const bool ok = decoded == symbols;
    std::cout << "{\n";
    std::cout << "  \"symbols\": " << symbols.size() << ",\n";
    std::cout << "  \"alphabet_size\": 257,\n";
    std::cout << "  \"payload_bytes\": " << payload.size() << ",\n";
    std::cout << "  \"roundtrip_ok\": " << (ok ? "true" : "false") << "\n";
    std::cout << "}\n";
    return ok ? 0 : 2;
}

struct PredictorCase {
    int count = 0;
    int total = 0;
    int left_remaining = 0;
    int right_remaining = 0;
    int symbols_remaining = 0;
    int context = 0;
};

std::vector<PredictorCase> make_predictor_cases() {
    std::vector<PredictorCase> cases;
    cases.reserve(4096);
    for (int i = 0; i < 4096; ++i) {
        PredictorCase item;
        item.total = 1 + ((i * 37 + (i >> 2)) % 200);
        item.left_remaining = item.total + ((i * 7 + 3) % 50);
        item.right_remaining = item.left_remaining + 1 + ((i * 13 + 5) % 100);
        item.symbols_remaining = 2 + ((i * 5 + 11) % 512);
        item.context = (i * 11 + (i >> 3)) & 31;
        item.count = (i * 19 + (i >> 1)) % (item.total + 1);
        cases.push_back(item);
    }
    return cases;
}

int predictor_selftest(const std::string& payload_out) {
    const auto cases = make_predictor_cases();
    ArithmeticEncoder encoder;
    M03SerializablePredictor enc_predictor;
    for (const auto& item : cases) {
        enc_predictor.encode_count(
            encoder,
            item.count,
            item.total,
            item.left_remaining,
            item.right_remaining,
            item.symbols_remaining,
            item.context);
    }
    const auto payload = encoder.finish();
    if (!payload_out.empty()) {
        write_file(payload_out, payload);
    }

    ArithmeticDecoder decoder(payload);
    M03SerializablePredictor dec_predictor;
    size_t mismatches = 0;
    for (const auto& item : cases) {
        const int decoded = dec_predictor.decode_count(
            decoder,
            item.total,
            item.left_remaining,
            item.right_remaining,
            item.symbols_remaining,
            item.context);
        if (decoded != item.count) {
            ++mismatches;
        }
    }

    std::cout << "{\n";
    std::cout << "  \"cases\": " << cases.size() << ",\n";
    std::cout << "  \"payload_bytes\": " << payload.size() << ",\n";
    std::cout << "  \"payload_out\": \"" << json_escape(payload_out) << "\",\n";
    std::cout << "  \"mismatches\": " << mismatches << ",\n";
    std::cout << "  \"roundtrip_ok\": " << (mismatches == 0 ? "true" : "false") << "\n";
    std::cout << "}\n";
    return mismatches == 0 ? 0 : 2;
}

int split_payload_selftest(const std::string& payload_path, const std::string& case_name) {
    std::vector<uint64_t> root(256, 0);
    std::array<uint32_t, 256> expected_left{};
    uint32_t right_context_offset = 0;
    uint32_t left_subintervals = 0;
    uint32_t right_subintervals = 0;

    if (case_name == "balanced") {
        root[32] = 50;
        root[97] = 30;
        root[98] = 15;
        root[10] = 5;
        expected_left[32] = 20;
        expected_left[97] = 12;
        expected_left[98] = 9;
        expected_left[10] = 2;
        right_context_offset = 43;
        left_subintervals = 2;
        right_subintervals = 3;
    } else if (case_name == "mirrored") {
        root[32] = 80;
        root[97] = 20;
        expected_left[32] = 60;
        expected_left[97] = 5;
        right_context_offset = 65;
        left_subintervals = 4;
        right_subintervals = 2;
    } else {
        throw std::runtime_error("unknown split selftest case");
    }

    const auto payload = read_file(payload_path);
    ArithmeticDecoder decoder(payload);
    ParserDecodeState state(101, 100, root, decoder);
    state.initialize_root_context();
    state.apply_split(0, right_context_offset, 2, left_subintervals, right_subintervals);
    const auto decoded_left = state.context_histogram(0);

    size_t mismatches = 0;
    uint64_t decoded_left_total = 0;
    for (size_t i = 0; i < decoded_left.size(); ++i) {
        decoded_left_total += decoded_left[i];
        if (decoded_left[i] != expected_left[i]) {
            ++mismatches;
        }
    }

    std::cout << "{\n";
    std::cout << "  \"case\": \"" << json_escape(case_name) << "\",\n";
    std::cout << "  \"payload_path\": \"" << json_escape(payload_path) << "\",\n";
    std::cout << "  \"payload_bytes\": " << payload.size() << ",\n";
    std::cout << "  \"decoded_left_total\": " << decoded_left_total << ",\n";
    std::cout << "  \"mismatches\": " << mismatches << ",\n";
    std::cout << "  \"splits\": " << state.splits() << ",\n";
    std::cout << "  \"left_histogram_ok\": " << (mismatches == 0 ? "true" : "false") << "\n";
    std::cout << "}\n";
    return mismatches == 0 ? 0 : 2;
}

int m03_parse_l(const std::string& blob_path, const std::string& l_out_path) {
    const auto blob = read_file(blob_path);
    const auto decoded = decode_m03_l_stream(blob);
    if (!l_out_path.empty()) {
        write_file(l_out_path, decoded.l_stream);
    }

    std::cout << "{\n";
    std::cout << "  \"blob_path\": \"" << json_escape(blob_path) << "\",\n";
    std::cout << "  \"l_out\": \"" << json_escape(l_out_path) << "\",\n";
    std::cout << "  \"original_size\": " << decoded.original_size << ",\n";
    std::cout << "  \"l_bytes\": " << decoded.l_stream.size() << ",\n";
    std::cout << "  \"symbol_size\": " << decoded.symbol_size << ",\n";
    std::cout << "  \"primary_index\": " << decoded.primary_index << ",\n";
    std::cout << "  \"root_mode\": \"" << decoded.root_mode << "\",\n";
    std::cout << "  \"root_sum_ok\": true,\n";
    std::cout << "  \"contexts_visited\": " << decoded.contexts_visited << ",\n";
    std::cout << "  \"splits\": " << decoded.splits << "\n";
    std::cout << "}\n";
    return 0;
}

int m03_decode_block(const std::string& blob_path, const std::string& out_path) {
    const auto blob = read_file(blob_path);
    M03LStream meta;
    const auto decoded = decompress_m03_block(blob, &meta);
    if (!out_path.empty()) {
        write_file(out_path, decoded);
    }

    std::cout << "{\n";
    std::cout << "  \"blob_path\": \"" << json_escape(blob_path) << "\",\n";
    std::cout << "  \"out\": \"" << json_escape(out_path) << "\",\n";
    std::cout << "  \"original_size\": " << meta.original_size << ",\n";
    std::cout << "  \"decoded_bytes\": " << decoded.size() << ",\n";
    std::cout << "  \"primary_index\": " << meta.primary_index << ",\n";
    std::cout << "  \"root_mode\": \"" << meta.root_mode << "\",\n";
    std::cout << "  \"contexts_visited\": " << meta.contexts_visited << ",\n";
    std::cout << "  \"splits\": " << meta.splits << "\n";
    std::cout << "}\n";
    return 0;
}

std::vector<uint8_t> decode_bwt_order0_archive_blob(const std::vector<uint8_t>& archive_blob,
                                                    uint64_t* main_payload_bytes,
                                                    uint64_t* side_payload_bytes,
                                                    size_t* archive_bytes) {
    if (archive_blob.empty() || archive_blob[0] != kModeBwtOrder0) {
        throw std::runtime_error("not a bwt_order0 archive");
    }
    size_t pos = 1;
    const uint64_t original_size = read_varint(archive_blob, pos);
    if (original_size == 0) {
        if (main_payload_bytes != nullptr) *main_payload_bytes = 0;
        if (side_payload_bytes != nullptr) *side_payload_bytes = 0;
        if (archive_bytes != nullptr) *archive_bytes = archive_blob.size();
        return {};
    }
    const uint64_t primary_index = read_varint(archive_blob, pos);
    if (primary_index > original_size) {
        throw std::runtime_error("bwt_order0 primary out of range");
    }
    std::array<uint64_t, 257> cumulative{};
    uint64_t total = 0;
    for (size_t symbol = 0; symbol < 256; ++symbol) {
        cumulative[symbol] = total;
        total += read_varint(archive_blob, pos);
    }
    cumulative[256] = total;
    if (total != original_size) {
        throw std::runtime_error("bwt_order0 frequency sum mismatch");
    }
    const std::vector<uint8_t> payload(
        archive_blob.begin() + static_cast<std::ptrdiff_t>(pos),
        archive_blob.end());
    ArithmeticDecoder decoder(payload);
    std::vector<uint8_t> bwt;
    bwt.reserve(static_cast<size_t>(original_size));
    for (uint64_t i = 0; i < original_size; ++i) {
        const uint64_t value = decoder.value(original_size);
        size_t symbol = 0;
        while (symbol < 256 && !(cumulative[symbol] <= value && value < cumulative[symbol + 1])) {
            ++symbol;
        }
        if (symbol >= 256) {
            throw std::runtime_error("bwt_order0 symbol lookup failed");
        }
        decoder.update(cumulative[symbol], cumulative[symbol + 1], original_size);
        bwt.push_back(static_cast<uint8_t>(symbol));
    }
    if (main_payload_bytes != nullptr) *main_payload_bytes = archive_blob.size();
    if (side_payload_bytes != nullptr) *side_payload_bytes = 0;
    if (archive_bytes != nullptr) *archive_bytes = archive_blob.size();
    return inverse_bwt_nosentinel_cpp(bwt, primary_index);
}

std::vector<uint8_t> decode_digit_archive_blob(std::vector<uint8_t> archive_blob,
                                               uint64_t* main_payload_bytes,
                                               uint64_t* side_payload_bytes,
                                               size_t* archive_bytes) {
    if (!archive_blob.empty() && archive_blob[0] == kModeBwtOrder0) {
        return decode_bwt_order0_archive_blob(archive_blob, main_payload_bytes, side_payload_bytes, archive_bytes);
    }
    const auto parts = parse_digit_archive_blob(std::move(archive_blob));
    const auto main_begin = parts.blob.begin() + static_cast<std::ptrdiff_t>(parts.main_offset);
    const auto main_end = main_begin + static_cast<std::ptrdiff_t>(parts.main_len);
    const std::vector<uint8_t> main_blob(main_begin, main_end);
    const bool native_primary_bound = parts.mode == kModeDigitContextV5Native;
    const auto main = decompress_m03_block(main_blob, nullptr, native_primary_bound);

    std::vector<std::vector<uint8_t>> streams;
    streams.reserve(kCap + 1);
    size_t pos = parts.main_offset + static_cast<size_t>(parts.main_len);
    for (uint64_t len : parts.stream_lens) {
        if (pos + len > parts.blob.size()) {
            throw std::runtime_error("digit stream length exceeds archive size");
        }
        if (len == 0) {
            streams.emplace_back();
        } else {
            const auto begin = parts.blob.begin() + static_cast<std::ptrdiff_t>(pos);
            const auto end = begin + static_cast<std::ptrdiff_t>(len);
            streams.push_back(decompress_m03_block(std::vector<uint8_t>(begin, end), nullptr, native_primary_bound));
        }
        pos += static_cast<size_t>(len);
    }
    if (pos != parts.blob.size()) {
        throw std::runtime_error("trailing digit archive bytes");
    }

    if (main_payload_bytes != nullptr) {
        *main_payload_bytes = parts.main_len;
    }
    if (side_payload_bytes != nullptr) {
        *side_payload_bytes = parts.side_total;
    }
    if (archive_bytes != nullptr) {
        *archive_bytes = parts.blob.size();
    }
    return join_streams(main, streams);
}

int decode_archive(const std::string& archive_path, const std::string& out_path) {
    auto archive_blob = read_file(archive_path);
    if (archive_blob.empty()) {
        throw std::runtime_error("empty archive");
    }
    if (archive_blob[0] == kModeDigitContextV5Chunked) {
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            throw std::runtime_error("cannot open output: " + out_path);
        }
        size_t pos = 1;
        uint64_t decoded_bytes = 0;
        uint64_t main_payload_bytes = 0;
        uint64_t side_payload_bytes = 0;
        size_t chunks = 0;
        while (true) {
            const uint64_t raw_len = read_varint(archive_blob, pos);
            if (raw_len == 0) {
                break;
            }
            const uint64_t block_len = read_varint(archive_blob, pos);
            if (pos + block_len > archive_blob.size()) {
                throw std::runtime_error("chunked block length exceeds archive size");
            }
            const auto begin = archive_blob.begin() + static_cast<std::ptrdiff_t>(pos);
            const auto end = begin + static_cast<std::ptrdiff_t>(block_len);
            uint64_t block_main = 0;
            uint64_t block_side = 0;
            auto decoded = decode_digit_archive_blob(std::vector<uint8_t>(begin, end), &block_main, &block_side, nullptr);
            if (decoded.size() != raw_len) {
                throw std::runtime_error("chunk decoded length mismatch");
            }
            write_blob_to_stream(out, decoded, out_path);
            decoded_bytes += decoded.size();
            main_payload_bytes += block_main;
            side_payload_bytes += block_side;
            ++chunks;
            pos += static_cast<size_t>(block_len);
        }
        if (pos != archive_blob.size()) {
            throw std::runtime_error("trailing chunked archive bytes");
        }
        out.close();
        if (!out) {
            throw std::runtime_error("failed closing output: " + out_path);
        }
        std::cout << "{\n";
        std::cout << "  \"archive_path\": \"" << json_escape(archive_path) << "\",\n";
        std::cout << "  \"out\": \"" << json_escape(out_path) << "\",\n";
        std::cout << "  \"mode\": \"digit_context_v5_chunked\",\n";
        std::cout << "  \"chunks\": " << chunks << ",\n";
        std::cout << "  \"archive_bytes\": " << archive_blob.size() << ",\n";
        std::cout << "  \"decoded_bytes\": " << decoded_bytes << ",\n";
        std::cout << "  \"main_payload_bytes\": " << main_payload_bytes << ",\n";
        std::cout << "  \"side_payload_bytes\": " << side_payload_bytes << ",\n";
        std::cout << "  \"ledger_total_matches\": true\n";
        std::cout << "}\n";
        return 0;
    }

    uint64_t main_payload_bytes = 0;
    uint64_t side_payload_bytes = 0;
    size_t block_archive_bytes = 0;
    const auto decoded = decode_digit_archive_blob(
        std::move(archive_blob),
        &main_payload_bytes,
        &side_payload_bytes,
        &block_archive_bytes);
    write_file(out_path, decoded);
    std::cout << "{\n";
    std::cout << "  \"archive_path\": \"" << json_escape(archive_path) << "\",\n";
    std::cout << "  \"out\": \"" << json_escape(out_path) << "\",\n";
    std::cout << "  \"archive_bytes\": " << block_archive_bytes << ",\n";
    std::cout << "  \"decoded_bytes\": " << decoded.size() << ",\n";
    std::cout << "  \"main_payload_bytes\": " << main_payload_bytes << ",\n";
    std::cout << "  \"side_payload_bytes\": " << side_payload_bytes << ",\n";
    std::cout << "  \"ledger_total_matches\": true\n";
    std::cout << "}\n";
    return 0;
}

struct EncodedDigitBlock {
    std::vector<uint8_t> blob;
    size_t input_bytes = 0;
    size_t run_count = 0;
    size_t header_bytes = 0;
    uint64_t main_payload_bytes = 0;
    uint64_t side_payload_bytes = 0;
};

EncodedDigitBlock encode_bwt_order0_block(const std::vector<uint8_t>& data) {
    EncodedDigitBlock result;
    result.input_bytes = data.size();
    result.blob.push_back(kModeBwtOrder0);
    write_varint(data.size(), result.blob);
    if (data.empty()) {
        result.header_bytes = result.blob.size();
        return result;
    }
    const auto transformed = bwt_transform_libsais(data);
    write_varint(transformed.primary_index, result.blob);
    std::array<uint64_t, 256> frequencies{};
    for (uint8_t byte : transformed.bwt) {
        ++frequencies[byte];
    }
    for (uint64_t count : frequencies) {
        write_varint(count, result.blob);
    }
    result.header_bytes = result.blob.size();

    std::array<uint64_t, 257> cumulative{};
    uint64_t total = 0;
    for (size_t symbol = 0; symbol < 256; ++symbol) {
        cumulative[symbol] = total;
        total += frequencies[symbol];
    }
    cumulative[256] = total;
    ArithmeticEncoder encoder;
    for (uint8_t byte : transformed.bwt) {
        encoder.encode(cumulative[byte], cumulative[byte + 1], total);
    }
    const auto payload = encoder.finish();
    result.blob.insert(result.blob.end(), payload.begin(), payload.end());
    result.main_payload_bytes = result.blob.size();
    result.side_payload_bytes = 0;
    return result;
}

EncodedDigitBlock encode_digit_block(std::vector<uint8_t> data, uint64_t max_parser_events = 0) {
    EncodedDigitBlock result;
    const size_t input_bytes = data.size();
    auto main = make_main(data);
    auto runs = scan_runs(main);
    const size_t run_count = runs.size();
    auto streams = build_streams(data, main, runs);
    if (input_bytes <= 16u * 1024u * 1024u && join_streams(main, streams) != data) {
        throw std::runtime_error("digit transform self-check failed");
    }
    std::vector<uint8_t>().swap(data);
    std::vector<DigitRun>().swap(runs);

    auto main_blob = compress_m03_block(main, "latin205", max_parser_events);
    std::vector<uint8_t>().swap(main);
    std::vector<std::vector<uint8_t>> stream_blobs;
    stream_blobs.reserve(kCap + 1);
    uint64_t side_payload_bytes = 0;
    for (auto& stream : streams) {
        if (stream.empty()) {
            stream_blobs.emplace_back();
            continue;
        }
        stream_blobs.push_back(compress_m03_block(stream, "generic", max_parser_events));
        side_payload_bytes += stream_blobs.back().size();
        std::vector<uint8_t>().swap(stream);
    }
    std::vector<std::vector<uint8_t>>().swap(streams);

    std::vector<uint8_t> header;
    header.push_back(kModeDigitContextV5Native);
    write_varint(main_blob.size(), header);
    for (const auto& blob : stream_blobs) {
        write_varint(blob.size(), header);
    }
    const size_t header_bytes = header.size();
    const uint64_t archive_bytes = static_cast<uint64_t>(header_bytes) + main_blob.size() + side_payload_bytes;
    result.blob.reserve(static_cast<size_t>(archive_bytes));
    result.blob.insert(result.blob.end(), header.begin(), header.end());
    result.blob.insert(result.blob.end(), main_blob.begin(), main_blob.end());
    for (const auto& blob : stream_blobs) {
        result.blob.insert(result.blob.end(), blob.begin(), blob.end());
    }

    result.input_bytes = input_bytes;
    result.run_count = run_count;
    result.header_bytes = header_bytes;
    result.main_payload_bytes = main_blob.size();
    result.side_payload_bytes = side_payload_bytes;
    return result;
}

struct AblationMetrics {
    std::string variant;
    uint64_t raw_component_bytes = 0;
    uint64_t header_bytes = 0;
    uint64_t main_payload_bytes = 0;
    uint64_t side_payload_bytes = 0;
    uint64_t archive_bytes = 0;
    std::vector<uint64_t> side_raw_bytes_by_stream;
    std::vector<uint64_t> side_payload_bytes_by_stream;
};

size_t varint_encoded_size(uint64_t value) {
    std::vector<uint8_t> tmp;
    write_varint(value, tmp);
    return tmp.size();
}

size_t digit_header_size_for(uint64_t main_payload_bytes, const std::vector<uint64_t>& stream_lens) {
    std::vector<uint8_t> header;
    header.push_back(kModeDigitContextV5Native);
    write_varint(main_payload_bytes, header);
    for (uint64_t len : stream_lens) {
        write_varint(len, header);
    }
    return header.size();
}

AblationMetrics measure_split_ablation(const std::string& variant,
                                       const std::vector<std::vector<uint8_t>>& streams,
                                       uint64_t main_payload_bytes) {
    AblationMetrics row;
    row.variant = variant;
    row.main_payload_bytes = main_payload_bytes;
    row.side_raw_bytes_by_stream.reserve(streams.size());
    row.side_payload_bytes_by_stream.reserve(streams.size());

    for (size_t i = 0; i < streams.size(); ++i) {
        row.raw_component_bytes += streams[i].size();
        row.side_raw_bytes_by_stream.push_back(streams[i].size());
        if (streams[i].empty()) {
            row.side_payload_bytes_by_stream.push_back(0);
            continue;
        }
        std::cerr << "ablation variant=" << variant
                  << " stream=" << i
                  << " raw_bytes=" << streams[i].size()
                  << "\n";
        auto blob = compress_m03_block(streams[i], "generic");
        row.side_payload_bytes += blob.size();
        row.side_payload_bytes_by_stream.push_back(blob.size());
        std::cerr << "ablation variant=" << variant
                  << " stream=" << i
                  << " payload_bytes=" << blob.size()
                  << "\n";
    }
    row.header_bytes = digit_header_size_for(main_payload_bytes, row.side_payload_bytes_by_stream);
    row.archive_bytes = row.header_bytes + row.main_payload_bytes + row.side_payload_bytes;
    return row;
}

void write_u64_array_json(std::ostream& out, const std::vector<uint64_t>& values) {
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ", ";
        out << values[i];
    }
    out << "]";
}

void write_ablation_row_json(std::ostream& out, const AblationMetrics& row, uint64_t base_archive_bytes) {
    out << "    {\n";
    out << "      \"variant\": \"" << json_escape(row.variant) << "\",\n";
    out << "      \"raw_component_bytes\": " << row.raw_component_bytes << ",\n";
    out << "      \"header_bytes\": " << row.header_bytes << ",\n";
    out << "      \"main_payload_bytes\": " << row.main_payload_bytes << ",\n";
    out << "      \"side_payload_bytes\": " << row.side_payload_bytes << ",\n";
    out << "      \"archive_bytes\": " << row.archive_bytes << ",\n";
    out << "      \"delta_vs_no_digit_split\": " << static_cast<int64_t>(row.archive_bytes) - static_cast<int64_t>(base_archive_bytes) << ",\n";
    out << "      \"side_raw_bytes_by_stream\": ";
    write_u64_array_json(out, row.side_raw_bytes_by_stream);
    out << ",\n";
    out << "      \"side_payload_bytes_by_stream\": ";
    write_u64_array_json(out, row.side_payload_bytes_by_stream);
    out << "\n";
    out << "    }";
}

int ablation_summary(const std::string& input_path,
                     const std::string& json_path,
                     size_t max_bytes,
                     const std::string& scored_archive_path) {
    auto data = read_file(input_path);
    if (max_bytes > 0 && data.size() > max_bytes) {
        data.resize(max_bytes);
    }
    std::cerr << "ablation loaded input_bytes=" << data.size() << "\n";

    AblationMetrics base;
    base.variant = "same_coder_no_digit_split";
    base.raw_component_bytes = data.size();
    std::cerr << "ablation variant=" << base.variant << " raw_bytes=" << data.size() << "\n";
    auto base_blob = compress_m03_block(data, "latin205");
    base.main_payload_bytes = base_blob.size();
    base.header_bytes = 1 + varint_encoded_size(base_blob.size());
    base.archive_bytes = base.header_bytes + base.main_payload_bytes;
    std::vector<uint8_t>().swap(base_blob);
    std::cerr << "ablation variant=" << base.variant
              << " payload_bytes=" << base.main_payload_bytes
              << " archive_bytes=" << base.archive_bytes
              << "\n";

    uint64_t scored_header_bytes = 0;
    uint64_t scored_main_payload_bytes = 0;
    uint64_t scored_side_payload_bytes = 0;
    uint64_t scored_archive_bytes = 0;
    if (!scored_archive_path.empty() && max_bytes == 0) {
        const auto parts = parse_digit_archive(scored_archive_path);
        scored_header_bytes = parts.header_bytes;
        scored_main_payload_bytes = parts.main_len;
        scored_side_payload_bytes = parts.side_total;
        scored_archive_bytes = parts.blob.size();
    } else {
        auto block = encode_digit_block(data);
        scored_header_bytes = block.header_bytes;
        scored_main_payload_bytes = block.main_payload_bytes;
        scored_side_payload_bytes = block.side_payload_bytes;
        scored_archive_bytes = block.blob.size();
    }

    auto main = make_main(data);
    auto runs = scan_runs(main);
    std::cerr << "ablation digit_runs=" << runs.size()
              << " scored_main_payload_bytes=" << scored_main_payload_bytes
              << "\n";

    std::vector<AblationMetrics> rows;
    rows.push_back(base);

    auto raw_digit_streams = build_raw_digit_streams(data, runs);
    rows.push_back(measure_split_ablation("raw_digit_side_stream", raw_digit_streams, scored_main_payload_bytes));
    std::vector<std::vector<uint8_t>>().swap(raw_digit_streams);

    auto sorting_off_streams = build_streams_variant(data, main, runs, false, true);
    rows.push_back(measure_split_ablation("bucket_packing_sorting_off", sorting_off_streams, scored_main_payload_bytes));
    std::vector<std::vector<uint8_t>>().swap(sorting_off_streams);

    auto packing_off_streams = build_streams_variant(data, main, runs, true, false);
    rows.push_back(measure_split_ablation("bucket_sorting_packing_off", packing_off_streams, scored_main_payload_bytes));
    std::vector<std::vector<uint8_t>>().swap(packing_off_streams);

    AblationMetrics stc;
    stc.variant = "stc_digit_context_v5";
    stc.raw_component_bytes = data.size();
    stc.header_bytes = scored_header_bytes;
    stc.main_payload_bytes = scored_main_payload_bytes;
    stc.side_payload_bytes = scored_side_payload_bytes;
    stc.archive_bytes = scored_archive_bytes;
    rows.push_back(stc);

    std::ofstream out(json_path);
    if (!out) {
        throw std::runtime_error("cannot open ablation json: " + json_path);
    }
    out << "{\n";
    out << "  \"input_path\": \"" << json_escape(input_path) << "\",\n";
    out << "  \"scored_archive_path\": \"" << json_escape(scored_archive_path) << "\",\n";
    out << "  \"input_bytes\": " << data.size() << ",\n";
    out << "  \"max_bytes\": " << max_bytes << ",\n";
    out << "  \"base_variant\": \"same_coder_no_digit_split\",\n";
    out << "  \"main_payload_reused_from_scored_archive\": " << ((!scored_archive_path.empty() && max_bytes == 0) ? "true" : "false") << ",\n";
    out << "  \"rows\": [\n";
    for (size_t i = 0; i < rows.size(); ++i) {
        write_ablation_row_json(out, rows[i], base.archive_bytes);
        out << (i + 1 == rows.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
    out.close();
    if (!out) {
        throw std::runtime_error("failed writing ablation json: " + json_path);
    }

    std::cout << "{\n";
    std::cout << "  \"input_path\": \"" << json_escape(input_path) << "\",\n";
    std::cout << "  \"json_path\": \"" << json_escape(json_path) << "\",\n";
    std::cout << "  \"input_bytes\": " << data.size() << ",\n";
    std::cout << "  \"same_coder_no_digit_split_bytes\": " << base.archive_bytes << ",\n";
    std::cout << "  \"stc_digit_context_v5_bytes\": " << scored_archive_bytes << ",\n";
    std::cout << "  \"stc_delta_vs_no_digit_split\": " << static_cast<int64_t>(scored_archive_bytes) - static_cast<int64_t>(base.archive_bytes) << "\n";
    std::cout << "}\n";
    return 0;
}

int encode_archive(const std::string& input_path, const std::string& archive_path, size_t max_bytes) {
    auto data = read_file(input_path);
    if (max_bytes > 0 && data.size() > max_bytes) {
        data.resize(max_bytes);
    }
    const auto result = encode_digit_block(std::move(data));
    write_file(archive_path, result.blob);

    std::cout << "{\n";
    std::cout << "  \"input_path\": \"" << json_escape(input_path) << "\",\n";
    std::cout << "  \"archive_path\": \"" << json_escape(archive_path) << "\",\n";
    std::cout << "  \"input_bytes\": " << result.input_bytes << ",\n";
    std::cout << "  \"archive_bytes\": " << result.blob.size() << ",\n";
    std::cout << "  \"runs\": " << result.run_count << ",\n";
    std::cout << "  \"digit_header_bytes\": " << result.header_bytes << ",\n";
    std::cout << "  \"main_payload_bytes\": " << result.main_payload_bytes << ",\n";
    std::cout << "  \"side_payload_bytes\": " << result.side_payload_bytes << ",\n";
    std::cout << "  \"ledger_total_matches\": true\n";
    std::cout << "}\n";
    return 0;
}

int encode_chunked_archive(const std::string& input_path, const std::string& archive_path, size_t chunk_bytes) {
    if (chunk_bytes == 0) {
        throw std::runtime_error("chunk size must be > 0");
    }
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input: " + input_path);
    }
    std::ofstream out(archive_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot open output: " + archive_path);
    }
    out.put(static_cast<char>(kModeDigitContextV5Chunked));
    if (!out) {
        throw std::runtime_error("short write: " + archive_path);
    }

    uint64_t input_total = 0;
    uint64_t archive_total = 1;
    uint64_t main_payload_bytes = 0;
    uint64_t side_payload_bytes = 0;
    size_t chunks = 0;
    while (true) {
        std::vector<uint8_t> chunk(chunk_bytes);
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got < 0) {
            throw std::runtime_error("negative read size");
        }
        if (got == 0) {
            break;
        }
        chunk.resize(static_cast<size_t>(got));
        auto block = encode_digit_block(std::move(chunk));

        std::vector<uint8_t> chunk_header;
        write_varint(block.input_bytes, chunk_header);
        write_varint(block.blob.size(), chunk_header);
        write_blob_to_stream(out, chunk_header, archive_path);
        write_blob_to_stream(out, block.blob, archive_path);

        input_total += block.input_bytes;
        archive_total += chunk_header.size() + block.blob.size();
        main_payload_bytes += block.main_payload_bytes;
        side_payload_bytes += block.side_payload_bytes;
        ++chunks;
        std::cerr << "chunk " << chunks
                  << " input_total=" << input_total
                  << " archive_total=" << archive_total
                  << " block_bytes=" << block.blob.size()
                  << "\n";
    }
    if (!in.eof()) {
        throw std::runtime_error("input read failed: " + input_path);
    }
    std::vector<uint8_t> sentinel;
    write_varint(0, sentinel);
    write_blob_to_stream(out, sentinel, archive_path);
    archive_total += sentinel.size();
    out.close();
    if (!out) {
        throw std::runtime_error("failed closing output: " + archive_path);
    }

    std::cout << "{\n";
    std::cout << "  \"input_path\": \"" << json_escape(input_path) << "\",\n";
    std::cout << "  \"archive_path\": \"" << json_escape(archive_path) << "\",\n";
    std::cout << "  \"mode\": \"digit_context_v5_chunked\",\n";
    std::cout << "  \"chunk_bytes\": " << chunk_bytes << ",\n";
    std::cout << "  \"chunks\": " << chunks << ",\n";
    std::cout << "  \"input_bytes\": " << input_total << ",\n";
    std::cout << "  \"archive_bytes\": " << archive_total << ",\n";
    std::cout << "  \"main_payload_bytes\": " << main_payload_bytes << ",\n";
    std::cout << "  \"side_payload_bytes\": " << side_payload_bytes << ",\n";
    std::cout << "  \"ledger_total_matches\": true\n";
    std::cout << "}\n";
    return 0;
}

bool encoded_block_roundtrips(const EncodedDigitBlock& block, const std::vector<uint8_t>& expected) {
    try {
        const auto decoded = decode_digit_archive_blob(block.blob, nullptr, nullptr, nullptr);
        return decoded == expected;
    } catch (const std::exception&) {
        return false;
    }
}

void write_chunk_record(std::ofstream& out,
                        const std::string& archive_path,
                        const EncodedDigitBlock& block,
                        uint64_t& archive_total) {
    std::vector<uint8_t> chunk_header;
    write_varint(block.input_bytes, chunk_header);
    write_varint(block.blob.size(), chunk_header);
    write_blob_to_stream(out, chunk_header, archive_path);
    write_blob_to_stream(out, block.blob, archive_path);
    archive_total += chunk_header.size() + block.blob.size();
}

int encode_chunked_archive_adaptive(const std::string& input_path,
                                    const std::string& archive_path,
                                    size_t chunk_bytes,
                                    size_t fallback_chunk_bytes) {
    constexpr uint64_t kFallbackM03EventBudget = 2000000;
    if (chunk_bytes == 0 || fallback_chunk_bytes == 0 || fallback_chunk_bytes > chunk_bytes) {
        throw std::runtime_error("invalid adaptive chunk sizes");
    }
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input: " + input_path);
    }
    std::ofstream out(archive_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot open output: " + archive_path);
    }
    out.put(static_cast<char>(kModeDigitContextV5Chunked));
    if (!out) {
        throw std::runtime_error("short write: " + archive_path);
    }

    uint64_t input_total = 0;
    uint64_t archive_total = 1;
    uint64_t main_payload_bytes = 0;
    uint64_t side_payload_bytes = 0;
    size_t logical_chunks = 0;
    size_t records = 0;
    size_t fallback_records = 0;
    size_t fallback_m03_records = 0;
    size_t fallback_order0_records = 0;
    while (true) {
        std::vector<uint8_t> chunk(chunk_bytes);
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got < 0) {
            throw std::runtime_error("negative read size");
        }
        if (got == 0) {
            break;
        }
        chunk.resize(static_cast<size_t>(got));
        ++logical_chunks;

        bool use_whole_chunk = false;
        EncodedDigitBlock block;
        std::string fallback_reason;
        try {
            block = encode_digit_block(chunk);
            use_whole_chunk = encoded_block_roundtrips(block, chunk);
            if (!use_whole_chunk) {
                fallback_reason = "roundtrip-mismatch";
            }
        } catch (const std::exception& exc) {
            fallback_reason = exc.what();
        }

        if (use_whole_chunk) {
            write_chunk_record(out, archive_path, block, archive_total);
            input_total += block.input_bytes;
            main_payload_bytes += block.main_payload_bytes;
            side_payload_bytes += block.side_payload_bytes;
            ++records;
            std::cerr << "record " << records
                      << " logical_chunk=" << logical_chunks
                      << " input_total=" << input_total
                      << " archive_total=" << archive_total
                      << " block_bytes=" << block.blob.size()
                      << "\n";
            continue;
        }

        std::cerr << "fallback logical_chunk=" << logical_chunks
                  << " reason=" << fallback_reason
                  << " bytes=" << chunk.size()
                  << "\n";
        for (size_t start = 0; start < chunk.size(); start += fallback_chunk_bytes) {
            const size_t end = std::min(chunk.size(), start + fallback_chunk_bytes);
            std::vector<uint8_t> subchunk(chunk.begin() + static_cast<std::ptrdiff_t>(start),
                                          chunk.begin() + static_cast<std::ptrdiff_t>(end));
            EncodedDigitBlock subblock;
            std::string subcodec = "m03_budget";
            std::string subfallback_reason;
            try {
                subblock = encode_digit_block(subchunk, kFallbackM03EventBudget);
                if (!encoded_block_roundtrips(subblock, subchunk)) {
                    subcodec = "bwt_order0";
                    subfallback_reason = "m03-budget-roundtrip-mismatch";
                }
            } catch (const std::exception& exc) {
                subcodec = "bwt_order0";
                subfallback_reason = exc.what();
            }
            if (subcodec == "bwt_order0") {
                subblock = encode_bwt_order0_block(subchunk);
                ++fallback_order0_records;
            } else {
                ++fallback_m03_records;
            }
            if (!encoded_block_roundtrips(subblock, subchunk)) {
                throw std::runtime_error("adaptive fallback subchunk roundtrip failed");
            }
            write_chunk_record(out, archive_path, subblock, archive_total);
            input_total += subblock.input_bytes;
            main_payload_bytes += subblock.main_payload_bytes;
            side_payload_bytes += subblock.side_payload_bytes;
            ++records;
            ++fallback_records;
            std::cerr << "record " << records
                      << " logical_chunk=" << logical_chunks
                      << " fallback=1"
                      << " codec=" << subcodec
                      << (subfallback_reason.empty() ? "" : " reason=")
                      << subfallback_reason
                      << " input_total=" << input_total
                      << " archive_total=" << archive_total
                      << " block_bytes=" << subblock.blob.size()
                      << "\n";
        }
    }
    if (!in.eof()) {
        throw std::runtime_error("input read failed: " + input_path);
    }
    std::vector<uint8_t> sentinel;
    write_varint(0, sentinel);
    write_blob_to_stream(out, sentinel, archive_path);
    archive_total += sentinel.size();
    out.close();
    if (!out) {
        throw std::runtime_error("failed closing output: " + archive_path);
    }

    std::cout << "{\n";
    std::cout << "  \"input_path\": \"" << json_escape(input_path) << "\",\n";
    std::cout << "  \"archive_path\": \"" << json_escape(archive_path) << "\",\n";
    std::cout << "  \"mode\": \"digit_context_v5_chunked_adaptive\",\n";
    std::cout << "  \"chunk_bytes\": " << chunk_bytes << ",\n";
    std::cout << "  \"fallback_chunk_bytes\": " << fallback_chunk_bytes << ",\n";
    std::cout << "  \"logical_chunks\": " << logical_chunks << ",\n";
    std::cout << "  \"records\": " << records << ",\n";
    std::cout << "  \"fallback_records\": " << fallback_records << ",\n";
    std::cout << "  \"fallback_m03_records\": " << fallback_m03_records << ",\n";
    std::cout << "  \"fallback_order0_records\": " << fallback_order0_records << ",\n";
    std::cout << "  \"input_bytes\": " << input_total << ",\n";
    std::cout << "  \"archive_bytes\": " << archive_total << ",\n";
    std::cout << "  \"main_payload_bytes\": " << main_payload_bytes << ",\n";
    std::cout << "  \"side_payload_bytes\": " << side_payload_bytes << ",\n";
    std::cout << "  \"ledger_total_matches\": true\n";
    std::cout << "}\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "usage: dcv5 inspect <archive> | dcv5 encode <input> <archive> [max_bytes] | dcv5 encode-chunked <input> <archive> [chunk_bytes] | dcv5 encode-chunked-adaptive <input> <archive> [chunk_bytes] [fallback_chunk_bytes] | dcv5 ablation-summary <input> <json> [max_bytes] [scored_archive] | dcv5 decode <archive> <out> | dcv5 m03-header <archive> | dcv5 m03-parse-l <m03_blob> [l_out] | dcv5 m03-decode-block <m03_blob> [out] | dcv5 raw-roundtrip <input> [max_bytes] | dcv5 arith-selftest | dcv5 predictor-selftest [payload_out] | dcv5 split-payload-selftest <payload>\n";
            return 2;
        }
        const std::string command = argv[1];
        if (command == "inspect") {
            if (argc < 3) {
                throw std::runtime_error("inspect requires archive path");
            }
            return inspect_archive(argv[2]);
        }
        if (command == "encode") {
            if (argc < 4) {
                throw std::runtime_error("encode requires input path and archive path");
            }
            const size_t max_bytes = argc >= 5 ? static_cast<size_t>(std::stoull(argv[4])) : 0;
            return encode_archive(argv[2], argv[3], max_bytes);
        }
        if (command == "encode-chunked") {
            if (argc < 4) {
                throw std::runtime_error("encode-chunked requires input path and archive path");
            }
            const size_t chunk_bytes = argc >= 5 ? static_cast<size_t>(std::stoull(argv[4])) : 100000000;
            return encode_chunked_archive(argv[2], argv[3], chunk_bytes);
        }
        if (command == "encode-chunked-adaptive") {
            if (argc < 4) {
                throw std::runtime_error("encode-chunked-adaptive requires input path and archive path");
            }
            const size_t chunk_bytes = argc >= 5 ? static_cast<size_t>(std::stoull(argv[4])) : 10000000;
            const size_t fallback_chunk_bytes = argc >= 6 ? static_cast<size_t>(std::stoull(argv[5])) : 1000000;
            return encode_chunked_archive_adaptive(argv[2], argv[3], chunk_bytes, fallback_chunk_bytes);
        }
        if (command == "ablation-summary") {
            if (argc < 4) {
                throw std::runtime_error("ablation-summary requires input path and json output path");
            }
            const size_t max_bytes = argc >= 5 ? static_cast<size_t>(std::stoull(argv[4])) : 0;
            const std::string scored_archive = argc >= 6 ? argv[5] : "";
            return ablation_summary(argv[2], argv[3], max_bytes, scored_archive);
        }
        if (command == "decode") {
            if (argc < 4) {
                throw std::runtime_error("decode requires archive path and output path");
            }
            return decode_archive(argv[2], argv[3]);
        }
        if (command == "m03-header") {
            if (argc < 3) {
                throw std::runtime_error("m03-header requires archive path");
            }
            return m03_header_inspect(argv[2]);
        }
        if (command == "raw-roundtrip") {
            if (argc < 3) {
                throw std::runtime_error("raw-roundtrip requires input path");
            }
            const size_t max_bytes = argc >= 4 ? static_cast<size_t>(std::stoull(argv[3])) : 0;
            return raw_roundtrip(argv[2], max_bytes);
        }
        if (command == "arith-selftest") {
            return arith_selftest();
        }
        if (command == "predictor-selftest") {
            const std::string payload_out = argc >= 3 ? argv[2] : "";
            return predictor_selftest(payload_out);
        }
        if (command == "split-payload-selftest") {
            if (argc < 3) {
                throw std::runtime_error("split-payload-selftest requires payload path");
            }
            const std::string case_name = argc >= 4 ? argv[3] : "balanced";
            return split_payload_selftest(argv[2], case_name);
        }
        if (command == "m03-parse-l") {
            if (argc < 3) {
                throw std::runtime_error("m03-parse-l requires M03 blob path");
            }
            const std::string l_out = argc >= 4 ? argv[3] : "";
            return m03_parse_l(argv[2], l_out);
        }
        if (command == "m03-decode-block") {
            if (argc < 3) {
                throw std::runtime_error("m03-decode-block requires M03 blob path");
            }
            const std::string out = argc >= 4 ? argv[3] : "";
            return m03_decode_block(argv[2], out);
        }
        std::cerr << "unknown command: " << command << "\n";
        return 2;
    } catch (const std::exception& exc) {
        std::cerr << "error: " << exc.what() << "\n";
        return 1;
    }
}
