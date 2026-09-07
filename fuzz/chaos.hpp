#pragma once
//
// Fuzzing harness with no coverage and no libFuzzer: this toolchain has no
// clang (libFuzzer is -fsanitize=fuzzer, a clang builtin gcc does not have),
// and the project does not download tools from the network to build.  This is
// the simple thing that works without either: a valid seed is mutated in
// random bites and each case runs in its own child process, so that a hang or
// an ASan/UBSan abort takes down only that case and the campaign carries on.
//
// It has no coverage guidance -- it does not know which case reaches further
// into the code -- but starting from real seeds instead of pure noise makes up
// for a good part of that: most mutations land near an input that already got
// past the lexer, so they reach the parser, the checker, and beyond.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace chaos {

using Reloj = std::chrono::steady_clock;

// A handful of mutations per case: bit flip, random byte, insert, delete,
// truncate, or splice with another seed.  Nothing fancy, on purpose.
inline std::string mutar(std::string s, const std::vector<std::string>& seeds,
                          std::mt19937& g) {
    if (s.empty()) s = " ";
    int pasadas = 1 + int(g() % 4);
    for (int p = 0; p < pasadas; ++p) {
        switch (g() % 6) {
            case 0: {
                size_t i = g() % s.size();
                s[i] = char(s[i] ^ (1 << (g() % 8)));
                break;
            }
            case 1: {
                size_t i = g() % s.size();
                s[i] = char(g() % 256);
                break;
            }
            case 2: {
                size_t i = g() % (s.size() + 1);
                s.insert(s.begin() + static_cast<long>(i), char(g() % 256));
                break;
            }
            case 3: {
                if (s.size() > 1) {
                    size_t i = g() % s.size();
                    s.erase(s.begin() + static_cast<long>(i));
                }
                break;
            }
            case 4: {
                size_t n = 1 + g() % s.size();
                s.resize(n);
                break;
            }
            case 5: {
                if (!seeds.empty()) {
                    const std::string& other = seeds[g() % seeds.size()];
                    if (!other.empty() && !s.empty()) {
                        size_t cut_a = g() % s.size();
                        size_t cut_b = g() % other.size();
                        s = s.substr(0, cut_a) + other.substr(cut_b);
                    }
                }
                break;
            }
        }
    }
    return s;
}

// Runs <target> over <iterations> mutated cases from <seeds>, each in its own
// child with a time cap.  Returns how many cases failed (crash or hang); each
// one is dumped to /tmp/<name>_fail_N.bin so it can be reproduced.
// aparte.
template <typename Fn>
int run(const char* name, int iterations, int limit_ms,
           const std::vector<std::string>& seeds, Fn target) {
    std::mt19937 g(std::random_device{}());
    int failures = 0;
    auto t0 = Reloj::now();

    for (int i = 0; i < iterations; ++i) {
        const std::string& base = seeds[g() % seeds.size()];
        std::string case_ = mutar(base, seeds, g);

        pid_t pid = fork();
        if (pid < 0) { std::perror("fork"); break; }
        if (pid == 0) {
            alarm(static_cast<unsigned>((limit_ms + 999) / 1000));
            target(case_);
            _exit(0);
        }

        int status = 0;
        waitpid(pid, &status, 0);

        bool crash = WIFSIGNALED(status);
        bool raro  = WIFEXITED(status) && WEXITSTATUS(status) != 0;
        if (crash || raro) {
            ++failures;
            std::string path = std::string("/tmp/") + name + "_fail_" +
                                std::to_string(failures) + ".bin";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(case_.data(), 1, case_.size(), f);
                std::fclose(f);
            }
            std::fprintf(stderr, "[%s] case_ %d: %s (%d) -- guardado en %s\n", name, i,
                          crash ? "signal" : "out", crash ? WTERMSIG(status) : WEXITSTATUS(status),
                          path.c_str());
        }

        if (i > 0 && i % 5000 == 0) {
            double s = std::chrono::duration<double>(Reloj::now() - t0).count();
            std::fprintf(stderr, "[%s] %d casos, %d failures, %.0f casos/s\n", name, i, failures,
                          i / s);
        }
    }

    double s = std::chrono::duration<double>(Reloj::now() - t0).count();
    std::fprintf(stderr, "[%s] fin: %d casos en %.1fs (%.0f/s), %d failures\n", name, iterations,
                  s, iterations / s, failures);
    return failures;
}

} // namespace chaos
