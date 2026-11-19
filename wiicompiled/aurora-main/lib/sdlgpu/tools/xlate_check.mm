// Translate every entry point of a WGSL file to MSL with aurora_wgsl_to_msl and compile it with Metal's
// runtime compiler (the one SDL_GPU uses). Prints the binding map, and the MSL on failure.
//
//   xlate_check FILE.wgsl [-v]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

extern "C" int aurora_wgsl_to_msl(const char* src, unsigned stage, const char* entry, char** out);

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: xlate_check FILE.wgsl [-v]\n");
    return 2;
  }
  const bool verbose = argc > 2 && std::strcmp(argv[2], "-v") == 0;
  std::ifstream in(argv[1]);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string src = ss.str();
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    std::fprintf(stderr, "no Metal device\n");
    return 2;
  }
  int failures = 0;
  const std::regex ep(R"(@(vertex|fragment|compute)[^\n]*\n?\s*fn\s+([A-Za-z_][A-Za-z0-9_]*))");
  for (auto it = std::sregex_iterator(src.begin(), src.end(), ep); it != std::sregex_iterator(); ++it) {
    const std::string kind = (*it)[1], name = (*it)[2];
    const unsigned stage = kind == "vertex" ? 0 : kind == "fragment" ? 1 : 2;
    char* out = nullptr;
    const int rc = aurora_wgsl_to_msl(src.c_str(), stage, name.c_str(), &out);
    std::string text = out ? out : "";
    std::free(out);
    if (rc != 0) {
      std::printf("FAIL translate %s %s: %s\n", kind.c_str(), name.c_str(), text.c_str());
      ++failures;
      continue;
    }
    const auto split = text.find("---\n");
    const std::string header = text.substr(0, split), msl = text.substr(split + 4);
    std::string entryName = name;
    std::istringstream hs(header);
    for (std::string line; std::getline(hs, line);) {
      if (line.rfind("entry ", 0) == 0) entryName = line.substr(6);
    }
    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()]
                                                  options:options
                                                    error:&error];
    id<MTLFunction> fn = library ? [library newFunctionWithName:[NSString stringWithUTF8String:entryName.c_str()]] : nil;
    if (!fn) {
      std::printf("FAIL metal %s %s: %s\n", kind.c_str(), name.c_str(),
                  error ? error.localizedDescription.UTF8String : "entry point missing");
      if (verbose) std::printf("%s\n", msl.c_str());
      ++failures;
      continue;
    }
    std::printf("ok   %s %s -> %s\n", kind.c_str(), name.c_str(), entryName.c_str());
    if (verbose) std::printf("%s", header.c_str());
  }
  return failures ? 1 : 0;
}
