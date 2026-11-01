#ifndef DATAGEN_HPP
#define DATAGEN_HPP

#include <string>
#include <cstdint>
#include <sstream>

namespace Datagen {

struct Config {
    int games = 100;
    int threads = 1;
    uint64_t nodes = 5000;
    int depth = 0;
    std::string outputPath = "data/selfplay.txt";
};

// Main entrypoints
void run(const Config& config);
void run_cli(std::istringstream& is);

} // namespace Datagen

#endif // DATAGEN_HPP
