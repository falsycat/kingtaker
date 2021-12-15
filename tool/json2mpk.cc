// Converts json file to msgpack
#include <iostream>

#include <nlohmann/json.hpp>

using namespace nlohmann;

int main(int argc, char** argv) {
  json j;
  std::cin >> j;

  const auto bytes = json::to_msgpack(j);
  for (size_t i = 0; i < bytes.size(); ) {
    for (size_t j = 0; i < bytes.size() && j < 16; ++j, ++i) {
      std::cout << "0x" << std::hex << static_cast<int>(bytes[i]) << ", ";
    }
    std::cout << "\n";
  }
  std::cout << std::endl;
  return 0;
}
