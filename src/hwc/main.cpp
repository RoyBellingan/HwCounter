#include <hwc/app.hpp>

#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: hwc serve|push [options]\n"
                     "  hwc serve --db data/hwc.sqlite --bind 0.0.0.0 --port 8080 "
                     "--token TOKEN --web web\n"
                     "  hwc push  --url http://host:8080 --token TOKEN <prefix>\n";
        return 2;
    }
    if (std::strcmp(argv[1], "serve") == 0) return hwc::cmd_serve(argc - 1, argv + 1);
    if (std::strcmp(argv[1], "push") == 0)  return hwc::cmd_push(argc - 1, argv + 1);
    std::cerr << "unknown command: " << argv[1] << "\n";
    return 2;
}
