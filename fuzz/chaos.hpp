#pragma once
//
// Arnes de fuzzing sin cobertura, sin libFuzzer: este toolchain no tiene clang
// (libFuzzer es -fsanitize=fuzzer, un builtin de clang que gcc no tiene), y el
// proyecto no baja herramientas de la red para compilar. Esto es lo simple que
// funciona sin ninguna de las dos cosas: se muta una semilla valida a bocados
// aleatorios y cada caso corre en un proceso hijo aparte, para que un cuelgue
// o un abort de ASan/UBSan tumbe solo ese caso y la campana siga.
//
// No tiene guia por cobertura -- no sabe que caso llega mas lejos en el
// codigo -- pero partir de semillas reales en vez de ruido puro compensa buena
// parte de eso: la mayoria de mutaciones caen cerca de una entrada que ya
// pasaba del lexer, así que llegan al parser, al checker, mas alla.

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

// Un puñado de mutaciones por caso: bit-flip, byte al azar, insertar, borrar,
// truncar, o empalmar con otra semilla. Nada elegante a proposito.
inline std::string mutar(std::string s, const std::vector<std::string>& semillas,
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
                if (!semillas.empty()) {
                    const std::string& otra = semillas[g() % semillas.size()];
                    if (!otra.empty() && !s.empty()) {
                        size_t corte_a = g() % s.size();
                        size_t corte_b = g() % otra.size();
                        s = s.substr(0, corte_a) + otra.substr(corte_b);
                    }
                }
                break;
            }
        }
    }
    return s;
}

// Corre <objetivo> sobre <iteraciones> casos mutados de <semillas>, cada uno en
// su propio hijo con un tope de tiempo. Devuelve cuantos casos fallaron (crash
// o cuelgue); cada uno se vuelca a /tmp/<nombre>_fallo_N.bin para reproducirlo
// aparte.
template <typename Fn>
int correr(const char* nombre, int iteraciones, int limite_ms,
           const std::vector<std::string>& semillas, Fn objetivo) {
    std::mt19937 g(std::random_device{}());
    int fallos = 0;
    auto t0 = Reloj::now();

    for (int i = 0; i < iteraciones; ++i) {
        const std::string& base = semillas[g() % semillas.size()];
        std::string caso = mutar(base, semillas, g);

        pid_t pid = fork();
        if (pid < 0) { std::perror("fork"); break; }
        if (pid == 0) {
            alarm(static_cast<unsigned>((limite_ms + 999) / 1000));
            objetivo(caso);
            _exit(0);
        }

        int estado = 0;
        waitpid(pid, &estado, 0);

        bool crash = WIFSIGNALED(estado);
        bool raro  = WIFEXITED(estado) && WEXITSTATUS(estado) != 0;
        if (crash || raro) {
            ++fallos;
            std::string ruta = std::string("/tmp/") + nombre + "_fallo_" +
                                std::to_string(fallos) + ".bin";
            if (FILE* f = std::fopen(ruta.c_str(), "wb")) {
                std::fwrite(caso.data(), 1, caso.size(), f);
                std::fclose(f);
            }
            std::fprintf(stderr, "[%s] caso %d: %s (%d) -- guardado en %s\n", nombre, i,
                          crash ? "señal" : "salida", crash ? WTERMSIG(estado) : WEXITSTATUS(estado),
                          ruta.c_str());
        }

        if (i > 0 && i % 5000 == 0) {
            double s = std::chrono::duration<double>(Reloj::now() - t0).count();
            std::fprintf(stderr, "[%s] %d casos, %d fallos, %.0f casos/s\n", nombre, i, fallos,
                          i / s);
        }
    }

    double s = std::chrono::duration<double>(Reloj::now() - t0).count();
    std::fprintf(stderr, "[%s] fin: %d casos en %.1fs (%.0f/s), %d fallos\n", nombre, iteraciones,
                  s, iteraciones / s, fallos);
    return fallos;
}

} // namespace chaos
