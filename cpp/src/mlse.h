#pragma once
// MLSE detector with a linear channel model fitted to every line, ported from
// the original Teletext Rescue (cpp/src/mlse.cpp). Three reads per bit
// (0.3, 0.5, 0.7), L bits before / R bits after in the model, Viterbi over
// 2^(L+R+1) states; the run-in and framing code are known bits. After the
// packet is read, bytes with a code (Hamming 8/4, odd parity) are re-decided
// among the valid code words against the fitted channel.
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace mlse {

constexpr int NP = 3; // reads per bit

struct Model {
    double spb = 0;             // samples per bit
    uint8_t pre[3] = {0x55, 0x55, 0x27}; // run-in + framing code, LSB first on the wire
    int nbytes = 34;            // data bytes after the framing code
    int L = 3, R = 3;           // neighbour bits in the model
    int ntop = 2;               // start offsets tried per line
    int refits = 1;             // refits of the model on the whole packet
    std::function<bool(const uint8_t*)> good;          // packet fit to train the model on
    std::function<int(const uint8_t*, int)> code;      // byte i: 0 any, 1 Hamming 8/4, 2 odd parity
    int NT() const { return 24 + nbytes * 8; }
    int NTAP() const { return L + R + 1; }
    int NS() const { return 1 << NTAP(); }
};

struct Result {
    std::vector<uint8_t> data; // nbytes, LSB first
    double off = 0, met = 0;
    bool good = false;
    std::vector<double> C;     // fitted channel (NP x (NTAP+1))
};

struct Pre;

class Engine {
public:
    explicit Engine(const Model& m);
    // One line, learning the prior from good packets (start-up, sequential).
    Result decodeLearn(const float* y, int n, double lo, double hi, double step);
    // One line against the current prior (thread safe; learn() afterwards).
    Result decode(const float* y, int n, double lo, double hi, double step) const;
    void learn(const Result& r);
    bool hasPrior() const { return !prior.empty(); }
    int learned() const { return n_; }
    const Model M;

private:
    std::shared_ptr<Pre> pre_;
    std::vector<double> prior;
    int n_ = 0;
};

// mean 0, deviation 1 over the line
void normalize(const float* in, int n, float* out);
// Hamming 8/4 codeword -> 0..15, -1 if not a codeword
int hamExact(uint8_t b);
bool oddParity(uint8_t b);
// the rule of WST rows: address/header bytes Hamming, text odd parity
int wstCode(const uint8_t* p, int i);

} // namespace mlse
