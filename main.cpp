/**
 * PoC: research integrity ledger using a hash chain (SHA-256)
 * Build: g++ -std=c++17 -O2 -Wall -o integrity main.cpp
 * Usage: ./integrity register <file> | verify | list
 */

#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

// SHA-256 (self-contained implementation, FIPS 180-4)
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

struct Sha256 {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t buf[64];
    size_t n = 0;
    uint64_t total = 0;

    static uint32_t rotr(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }

    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e,6) ^ rotr(e,11) ^ rotr(e,25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a,2) ^ rotr(a,13) ^ rotr(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    void update(const uint8_t* d, size_t len) {
        total += len;
        while (len--) { buf[n++] = *d++; if (n == 64) { block(buf); n = 0; } }
    }

    string final() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (n != 56) update(&pad, 1);
        for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); update(&b, 1); }
        ostringstream os;
        for (uint32_t v : h) os << hex << setw(8) << setfill('0') << v;
        return os.str();
    }
};

string sha256_str(const string& s) {
    Sha256 c;
    c.update((const uint8_t*)s.data(), s.size());
    return c.final();
}

string sha256_file(const string& path) {
    ifstream f(path, ios::binary);
    if (!f) return "";
    Sha256 c;
    vector<char> chunk(1 << 16);
    while (f.read(chunk.data(), chunk.size()) || f.gcount() > 0)
        c.update((const uint8_t*)chunk.data(), (size_t)f.gcount());
    return c.final();
}

// Ledger (hash chain)
const string LEDGER = "ledger.log";
const string GENESIS(64, '0');

struct Entry {
    size_t idx;
    long ts;
    string file, fhash, prev, hash;
    string compute() const {
        return sha256_str(to_string(idx) + "|" + to_string(ts) + "|" + file + "|" + fhash + "|" + prev);
    }
};

vector<Entry> load() {
    vector<Entry> v;
    ifstream f(LEDGER);
    string line;
    while (getline(f, line)) {
        vector<string> p;
        stringstream ss(line);
        string tok;
        while (getline(ss, tok, '|')) p.push_back(tok);
        if (p.size() != 6) { cerr << "Malformed line in ledger: " << line << "\n"; exit(2); }
        v.push_back({stoull(p[0]), stol(p[1]), p[2], p[3], p[4], p[5]});
    }
    return v;
}

int cmd_register(const string& path) {
    if (path.find('|') != string::npos) { cerr << "File name must not contain '|'\n"; return 2; }
    string fh = sha256_file(path);
    if (fh.empty()) { cerr << "Could not read: " << path << "\n"; return 2; }
    vector<Entry> v = load();
    Entry e{v.size(), (long)time(nullptr), path, fh, v.empty() ? GENESIS : v.back().hash, ""};
    e.hash = e.compute();
    ofstream(LEDGER, ios::app) << e.idx << "|" << e.ts << "|" << e.file << "|" << e.fhash << "|" << e.prev << "|" << e.hash << "\n";
    cout << "Registered #" << e.idx << "\n  file  : " << e.file << "\n  sha256: " << e.fhash
         << "\n  chain head: " << e.hash << "\n";
    return 0;
}

int cmd_verify() {
    vector<Entry> v = load();
    if (v.empty()) { cout << "Ledger is empty.\n"; return 0; }
    int problems = 0;
    for (size_t i = 0; i < v.size(); i++) {
        const Entry& e = v[i];
        string expectedPrev = (i == 0) ? GENESIS : v[i-1].hash;
        bool ok = true;
        if (e.idx != i)                { cout << "[#" << i << "] INDEX mismatch (entry removed/reordered?)\n"; ok = false; }
        if (e.prev != expectedPrev)    { cout << "[#" << i << "] CHAIN broken (previous hash does not match)\n"; ok = false; }
        if (e.compute() != e.hash)     { cout << "[#" << i << "] ENTRY tampered (entry hash does not match)\n"; ok = false; }
        string current = sha256_file(e.file);
        if (current.empty())             { cout << "[#" << i << "] FILE missing: " << e.file << "\n"; ok = false; }
        else if (current != e.fhash)     { cout << "[#" << i << "] FILE MODIFIED since registration: " << e.file << "\n"; ok = false; }
        if (ok) cout << "[#" << i << "] OK  " << e.file << "\n";
        else problems++;
    }
    cout << "\nChain head: " << v.back().hash << "\n";
    cout << (problems ? "RESULT: FAILED on " + to_string(problems) + " entry(ies).\n" : "RESULT: integrity confirmed.\n");
    return problems ? 1 : 0;
}

int cmd_list() {
    for (const Entry& e : load()) {
        char t[32];
        time_t ts = e.ts;
        strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S", localtime(&ts));
        cout << "#" << e.idx << "  " << t << "  " << e.file << "\n    sha256: " << e.fhash << "\n    chain : " << e.hash << "\n";
    }
    return 0;
}

int main(int argc, char** argv) {
    string c = argc > 1 ? argv[1] : "";
    if (c == "register" && argc == 3) return cmd_register(argv[2]);
    if (c == "verify" && argc == 2) return cmd_verify();
    if (c == "list" && argc == 2) return cmd_list();
    cerr << "Usage:\n  " << argv[0] << " register <file>\n  " << argv[0] << " verify\n  " << argv[0] << " list\n";
    return 2;
}
