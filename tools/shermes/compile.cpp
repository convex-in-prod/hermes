/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "compile.h"

#include "OutputStream.h"
#include "config.h"

#include "hermes/BCGen/SH/SH.h"

#include "llvh/ADT/ScopeExit.h"
#include "llvh/Support/MemoryBuffer.h"
#include "llvh/Support/Path.h"
#include "llvh/Support/Program.h"
#include "llvh/Support/Signals.h"

#include <algorithm>
#include <array>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define DEBUG_TYPE "shermesc"

using namespace hermes;

namespace {

class SHA256 {
  std::array<uint8_t, 64> data_{};
  size_t dataLength_{0};
  uint64_t bitLength_{0};
  std::array<uint32_t, 8> state_{
      0x6a09e667,
      0xbb67ae85,
      0x3c6ef372,
      0xa54ff53a,
      0x510e527f,
      0x9b05688c,
      0x1f83d9ab,
      0x5be0cd19,
  };

  static uint32_t rotateRight(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32 - count));
  }

  void transform() {
    static constexpr std::array<uint32_t, 64> constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    std::array<uint32_t, 64> words{};
    for (size_t i = 0; i < 16; ++i) {
      words[i] = (static_cast<uint32_t>(data_[i * 4]) << 24) |
          (static_cast<uint32_t>(data_[i * 4 + 1]) << 16) |
          (static_cast<uint32_t>(data_[i * 4 + 2]) << 8) |
          static_cast<uint32_t>(data_[i * 4 + 3]);
    }
    for (size_t i = 16; i < words.size(); ++i) {
      const uint32_t s0 = rotateRight(words[i - 15], 7) ^
          rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
      const uint32_t s1 = rotateRight(words[i - 2], 17) ^
          rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (size_t i = 0; i < words.size(); ++i) {
      const uint32_t sum1 =
          rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
      const uint32_t choice = (e & f) ^ (~e & g);
      const uint32_t temp1 = h + sum1 + choice + constants[i] + words[i];
      const uint32_t sum0 =
          rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

 public:
  void update(llvh::StringRef bytes) {
    for (uint8_t byte : bytes.bytes()) {
      data_[dataLength_++] = byte;
      if (dataLength_ == data_.size()) {
        transform();
        bitLength_ += 512;
        dataLength_ = 0;
      }
    }
  }

  std::string finalHex() {
    size_t index = dataLength_;
    data_[index++] = 0x80;
    if (index > 56) {
      std::fill(data_.begin() + index, data_.end(), 0);
      transform();
      index = 0;
    }
    std::fill(data_.begin() + index, data_.begin() + 56, 0);
    bitLength_ += dataLength_ * 8;
    for (size_t i = 0; i < 8; ++i)
      data_[63 - i] = static_cast<uint8_t>(bitLength_ >> (i * 8));
    transform();

    static constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < state_.size(); ++i) {
      for (size_t byte = 0; byte < 4; ++byte) {
        const uint8_t value =
            static_cast<uint8_t>(state_[i] >> (24 - byte * 8));
        result[(i * 4 + byte) * 2] = hex[value >> 4];
        result[(i * 4 + byte) * 2 + 1] = hex[value & 0x0f];
      }
    }
    return result;
  }
};

struct BundleFileIdentity {
  uint64_t size;
  std::string sha256;
};

llvh::Optional<BundleFileIdentity> identifyBundleFile(
    llvh::StringRef path) {
  auto file = llvh::MemoryBuffer::getFile(path, -1, false, false);
  if (!file) {
    llvh::errs() << "Failed to read generated bundle file " << path << ": "
                 << file.getError().message() << '\n';
    return llvh::None;
  }
  SHA256 hash;
  hash.update(file.get()->getBuffer());
  return BundleFileIdentity{
      file.get()->getBufferSize(),
      hash.finalHex(),
  };
}

/// Invoke the backend with the specified options. If the backend generates
/// an error (unlikely, but possible), print the number of errors and return
/// false.
bool invokeBackend(
    Context *context,
    Module &M,
    const BytecodeGenerationOptions &genOptions,
    llvh::raw_ostream &os) {
  assert(
      context->getSourceErrorManager().getErrorCount() == 0 &&
      "backend invocation with non-zero errors");
  if (auto N = context->getSourceErrorManager().getErrorCount()) {
    llvh::errs() << "Emitted " << N << " errors. exiting.\n";
    return false;
  }

  sh::generateSH(&M, os, genOptions);

  // Bail out if there were any errors during code generation.
  if (auto N = context->getSourceErrorManager().getErrorCount()) {
    llvh::errs() << "Emitted " << N << " errors. exiting.\n";
    return false;
  }

  return true;
}

/// Derive an output filename from the input filename by removing the path and
/// replacing the input extension with \p newExt, unless for some crazy reason
/// it already happens to be that.
///
/// \param input input filename
/// \param storage storage where the manipulated name is kept
/// \param newExt the new output extension
/// \return a reference to storage
llvh::StringRef deriveFilename(
    llvh::StringRef input,
    llvh::SmallString<32> &storage,
    llvh::StringRef newExt) {
  storage = llvh::sys::path::filename(input);
  if (llvh::sys::path::extension(storage) != newExt)
    llvh::sys::path::replace_extension(storage, newExt);
  else
    storage += newExt;
  return storage;
}

/// Invoke the backend when we are requested to "dump" one of our internal
/// representations. They are written to STDOUT by default, unless and output
/// path is specified.
bool compileToDump(
    hermes::Context *context,
    hermes::Module &M,
    const ShermesCompileParams &params,
    llvh::StringRef outputFilename) {
  OutputStream fileOS{};
  // If an output file name is not specified, use STDOUT.
  if (!fileOS.open(
          outputFilename.empty() ? "-" : outputFilename,
          llvh::sys::fs::F_None)) {
    return false;
  }
  if (!invokeBackend(context, M, params.genOptions, fileOS.os()))
    return false;
  return fileOS.close();
}

/// Invoke the backend to generate C.
bool compileToC(
    hermes::Context *context,
    hermes::Module &M,
    const ShermesCompileParams &params,
    llvh::StringRef inputFilename,
    llvh::StringRef outputFilename) {
  // If an output file name is not specified, derive it from the input.
  llvh::SmallString<32> outputPathBuf{};
  if (outputFilename.empty())
    outputFilename = deriveFilename(inputFilename, outputPathBuf, ".c");

  OutputStream fileOS{};
  if (!fileOS.open(outputFilename, llvh::sys::fs::F_None))
    return false;
  if (!invokeBackend(context, M, params.genOptions, fileOS.os()))
    return false;
  return fileOS.close();
}

const char *bundleFileRoleName(sh::SHCBundleFileRole role) {
  switch (role) {
    case sh::SHCBundleFileRole::Header:
      return "header";
    case sh::SHCBundleFileRole::Metadata:
      return "metadata";
    case sh::SHCBundleFileRole::Function:
      return "function";
  }
  llvm_unreachable("invalid Static Hermes C bundle file role");
}

const char *bundleOversizeReasonName(sh::SHCBundleOversizeReason reason) {
  switch (reason) {
    case sh::SHCBundleOversizeReason::None:
      return "none";
    case sh::SHCBundleOversizeReason::SingleInstruction:
      return "single-instruction";
    case sh::SHCBundleOversizeReason::NoOutlineableRun:
      return "no-outlineable-run";
  }
  llvm_unreachable("invalid Static Hermes C bundle oversize reason");
}

bool compileToCBundle(
    hermes::Context *context,
    hermes::Module &M,
    const ShermesCompileParams &params,
    llvh::StringRef inputFilename,
    llvh::StringRef outputFilename) {
  llvh::SmallString<32> outputPathBuf{};
  if (outputFilename.empty())
    outputFilename = deriveFilename(inputFilename, outputPathBuf, ".c.json");
  if (outputFilename == "-") {
    llvh::errs() << "A generated C bundle requires a manifest file path\n";
    return false;
  }
  const std::string generatedFilePrefix =
      ("sh_" + params.genOptions.unitName + "_").str();
  if (llvh::sys::path::filename(outputFilename).startswith(
          generatedFilePrefix)) {
    llvh::errs()
        << "The generated C bundle manifest name conflicts with its members\n";
    return false;
  }

  const llvh::StringRef outputDirectory =
      llvh::sys::path::parent_path(outputFilename);
  std::vector<sh::SHCBundleFile> files;
  const auto writeFile = [&](llvh::StringRef path,
                             llvh::function_ref<void(llvh::raw_ostream &)>
                                 emit) {
    llvh::SmallString<128> fullPath(outputDirectory);
    llvh::sys::path::append(fullPath, path);
    OutputStream output;
    if (!output.open(fullPath, llvh::sys::fs::F_None))
      return false;
    emit(output.os());
    return output.close();
  };

  assert(
      context->getSourceErrorManager().getErrorCount() == 0 &&
      "backend invocation with non-zero errors");
  const bool generated =
      sh::generateSHBundle(&M, writeFile, files, params.genOptions);
  if (auto count = context->getSourceErrorManager().getErrorCount()) {
    llvh::errs() << "Emitted " << count << " errors. exiting.\n";
    return false;
  }
  if (!generated)
    return false;
  if (files.size() < 3 || files[0].role != sh::SHCBundleFileRole::Header ||
      files[1].role != sh::SHCBundleFileRole::Metadata) {
    llvh::errs() << "Static Hermes emitted an invalid C bundle file list\n";
    return false;
  }
  uint32_t nextFunctionId = 0;
  for (size_t index = 2; index < files.size();) {
    const auto &file = files[index];
    if (file.role != sh::SHCBundleFileRole::Function) {
      llvh::errs() << "Static Hermes emitted an invalid C bundle file role\n";
      return false;
    }
    const bool invalidShard =
        file.functionCount == 0 ||
        file.lastFunctionId < file.firstFunctionId ||
        file.lastFunctionId - file.firstFunctionId + 1 !=
            file.functionCount ||
        file.firstFunctionId != nextFunctionId ||
        (file.oversize &&
         (file.functionCount != 1 ||
          file.oversizeReason == sh::SHCBundleOversizeReason::None)) ||
        (!file.oversize &&
         file.oversizeReason != sh::SHCBundleOversizeReason::None);
    if (invalidShard) {
      llvh::errs() << "Static Hermes emitted invalid function shard "
                      "oversize metadata\n";
      return false;
    }
    if (file.functionFragmentCount == 0) {
      nextFunctionId = file.lastFunctionId + 1;
      ++index;
      continue;
    }
    if (file.role != sh::SHCBundleFileRole::Function ||
        file.firstFunctionId != file.lastFunctionId ||
        file.functionCount != 1 ||
        file.functionFragmentIndex != 0 ||
        file.functionFragmentCount < 2) {
      llvh::errs() << "Static Hermes emitted invalid function fragments\n";
      return false;
    }
    if (file.functionFragmentCount > files.size() - index) {
      llvh::errs() << "Static Hermes emitted incomplete function fragments\n";
      return false;
    }

    // The backend emits a wrapper immediately followed by its helpers. Check
    // that group once: checking every fragment against the whole file list
    // would be quadratic at the 65,533-shard bundle limit.
    for (uint32_t part = 0; part < file.functionFragmentCount; ++part) {
      const auto &candidate = files[index + part];
      if (candidate.role != sh::SHCBundleFileRole::Function ||
          candidate.firstFunctionId != file.firstFunctionId ||
          candidate.lastFunctionId != file.firstFunctionId ||
          candidate.functionCount != 1 ||
          candidate.functionFragmentCount != file.functionFragmentCount ||
          candidate.functionFragmentIndex != part ||
          (candidate.oversize &&
           candidate.oversizeReason !=
               sh::SHCBundleOversizeReason::SingleInstruction) ||
          (!candidate.oversize &&
           candidate.oversizeReason != sh::SHCBundleOversizeReason::None)) {
        llvh::errs() << "Static Hermes emitted invalid function fragments\n";
        return false;
      }
    }
    nextFunctionId = file.firstFunctionId + 1;
    index += file.functionFragmentCount;
  }
  if (nextFunctionId != static_cast<uint32_t>(M.size())) {
    llvh::errs() << "Static Hermes emitted incomplete function shard coverage\n";
    return false;
  }

  std::vector<BundleFileIdentity> identities;
  identities.reserve(files.size());
  for (const auto &file : files) {
    llvh::SmallString<128> fullPath(outputDirectory);
    llvh::sys::path::append(fullPath, file.path);
    auto identity = identifyBundleFile(fullPath);
    if (!identity)
      return false;
    identities.push_back(std::move(*identity));
  }

  OutputStream manifest;
  if (!manifest.open(outputFilename, llvh::sys::fs::F_None))
    return false;
  auto &OS = manifest.os();
  const auto emitCommonFields = [&](size_t index) {
    const auto &file = files[index];
    const auto &identity = identities[index];
    OS << "\"path\":\"" << file.path << "\",\"role\":\""
       << bundleFileRoleName(file.role) << "\",\"sha256\":\""
       << identity.sha256 << "\",\"size\":" << identity.size;
  };
  OS << "{\"header\":{";
  emitCommonFields(0);
  OS << "},\"kind\":\"static-hermes-c-bundle-v1\",\"schemaVersion\":1,"
        "\"translationUnits\":[";
  for (size_t index = 1; index < files.size(); ++index) {
    if (index != 1)
      OS << ',';
    OS << '{';
    if (files[index].role == sh::SHCBundleFileRole::Function) {
      if (files[index].cOptimizationLevel ==
          sh::SHCBundleCOptimizationLevel::O0) {
        OS << "\"cOptimizationLevel\":0,";
      }
      OS << "\"firstFunctionId\":" << files[index].firstFunctionId
         << ",\"functionCount\":" << files[index].functionCount;
      if (files[index].functionFragmentCount != 0) {
        OS << ",\"functionFragmentCount\":"
           << files[index].functionFragmentCount
           << ",\"functionFragmentIndex\":"
           << files[index].functionFragmentIndex;
      }
      OS << ",\"lastFunctionId\":" << files[index].lastFunctionId
         << ",\"oversize\":" << (files[index].oversize ? "true" : "false")
         << ',';
      if (files[index].oversize) {
        OS << "\"oversizeReason\":\""
           << bundleOversizeReasonName(files[index].oversizeReason) << "\",";
      }
    }
    emitCommonFields(index);
    if (files[index].role == sh::SHCBundleFileRole::Function)
      OS << ",\"targetBytes\":" << files[index].targetBytes;
    OS << '}';
  }
  OS << "]}\n";
  return manifest.close();
}

/// Configuration for invoking the C compiler.
struct CCCfg {
  std::string cc;
  std::string syscflags;
  std::string sysldflags;
  std::string cflags;
  std::string ldflags;
  std::string ldlibs;
  std::vector<std::string> hermesLibPath;
  std::vector<std::string> hermesIncludePath;
};

/// Populate CCCfg with overrides from the environment.
void populateCCCfg(CCCfg &cfg) {
  auto init = [](std::string &res, const char *name, const char *defVal) {
    if (const char *t = ::getenv(name))
      res = t;
    else
      res = defVal;
  };

  init(cfg.cc, "CC", SHERMES_CC);
  cfg.syscflags = SHERMES_CC_SYSCFLAGS;
  cfg.sysldflags = SHERMES_CC_SYSLDFLAGS;
  init(cfg.cflags, "CFLAGS", "");
  init(cfg.ldflags, "LDFLAGS", "");
  init(cfg.ldlibs, "LDLIBS", "");

  llvh::SmallVector<llvh::StringRef, 2> vec{};
  llvh::StringLiteral(SHERMES_CC_LIB_PATH).split(vec, ':', -1, false);
  for (auto sr : vec)
    cfg.hermesLibPath.push_back(sr.str());

  vec.clear();
  llvh::StringLiteral(SHERMES_CC_INCLUDE_PATH).split(vec, ':', -1, false);
  for (auto sr : vec)
    cfg.hermesIncludePath.push_back(sr.str());
}

// Split arguments separated by whitespace and push them individually into
// `args`. Honor quotation marks.
static void splitArgs(llvh::StringRef str, std::vector<std::string> &args) {
  size_t size = str.size();
  size_t i = 0;
  // The current argument is accumulated here.
  std::string tmp{};
  while (i != size) {
    // Skip spaces
    if (isspace(str[i])) {
      ++i;
      continue;
    }

    tmp.clear();
    do {
      // Quoted sequences are copied without splitting.
      if (str[i] == '\'' || str[i] == '"') {
        size_t closingIndex = str.find(str[i], i + 1);
        if (closingIndex != llvh::StringRef::npos) {
          tmp.append(str.data() + i + 1, closingIndex - i - 1);
          i = closingIndex + 1;
          continue;
        }
      }

      tmp.push_back(str[i]);
      ++i;
    } while (i != size && !isspace(str[i]));
    args.push_back(tmp);
  }
}

/// Invoke the C compiler.
bool invokeCC(
    const ShermesCompileParams &params,
    OutputLevelKind outputLevel,
    llvh::StringRef inputPath,
    llvh::StringRef outputPath) {
  CCCfg cfg;
  populateCCCfg(cfg);

  auto res = llvh::sys::findProgramByName(cfg.cc);
  if (!res) {
    llvh::errs() << cfg.cc << ":" << res.getError().message() << "\n";
    return false;
  }
  llvh::StringRef program = *res;

  std::vector<std::string> args{};

  args.emplace_back(program);
  args.emplace_back(inputPath);

  // Select compilation to asm, obj, binary
  switch (outputLevel) {
    case OutputLevelKind::Asm:
      args.emplace_back("-S");
      break;
    case OutputLevelKind::Obj:
      args.emplace_back("-c");
      break;
    case OutputLevelKind::SharedObj:
      args.emplace_back("-fPIC");
#ifdef __APPLE__
      args.emplace_back("-dynamiclib");
#else
      args.emplace_back("-shared");
#endif
      break;
    case OutputLevelKind::Executable:
      break;
    default:
      hermes_fatal("unexpected output level");
  }

  // If CFLAGS were specified, they override our optimization level and include
  // path.
  if (cfg.cflags.empty()) {
    splitArgs(cfg.syscflags, args);
    switch (params.nativeOptimize) {
      case OptLevel::O0:
        break;
      case OptLevel::Og:
        args.emplace_back("-Og");
        break;
      case OptLevel::Os:
        args.emplace_back("-Os");
        break;
      case OptLevel::OMax:
        args.emplace_back("-O3");
        break;
    }
    for (const auto &s : cfg.hermesIncludePath)
      args.push_back("-I" + s);

    if (params.enableAsserts == ShermesCompileParams::EnableAsserts::off) {
      args.emplace_back("-DNDEBUG");
    }
    if (params.genOptions.emitLineDirectives) {
      args.emplace_back("-g");
    }
    // We depend on reading/writing certain properties in C++ objects in
    // generated C code through C structs that mirror those C++ objects. This
    // means that unrelated types may alias in our code, and we must disable
    // strict aliasing.
    args.emplace_back("-fno-strict-aliasing");
    // Avoid arbitrary UB on signed integer and pointer overflow.
    args.emplace_back("-fno-strict-overflow");
    for (llvh::StringRef option : params.extraCCOptions) {
      args.emplace_back(option);
    }
  } else {
    splitArgs(cfg.cflags, args);
  }

  // Append the library paths and library.
  if (outputLevel == OutputLevelKind::Executable ||
      outputLevel == OutputLevelKind::SharedObj) {
    for (const auto &s : params.libSearchPaths)
      args.push_back("-L" + s);
    for (const auto &s : params.libs)
      args.push_back("-l" + s);

    if (cfg.ldflags.empty()) {
      splitArgs(cfg.sysldflags, args);
      for (const auto &s : cfg.hermesLibPath)
        args.push_back("-L" + s);

      // If we are statically linking, we need to explicitly list Hermes'
      // external dependencies.
      if (params.staticLink == ShermesCompileParams::StaticLink::on) {
#ifdef __APPLE__
        args.emplace_back("-framework");
        args.emplace_back("CoreFoundation");
        args.emplace_back("-framework");
        args.emplace_back("Foundation");
        args.emplace_back("-lc++");
        args.emplace_back("-ljsi");
        if (!params.noHermesLibs) {
          args.emplace_back("-lshermes_console_a");
        }
#else
        llvh::errs() << "Static linking unsupported on this platform\n";
        return false;
#endif
      } else {
        if (!params.noHermesLibs) {
          args.emplace_back("-lshermes_console");
        }
        for (const auto &s : cfg.hermesLibPath) {
          args.emplace_back("-Wl,-rpath");
          args.emplace_back(s);
        }
        for (const auto &s : params.libSearchPaths) {
          args.emplace_back("-Wl,-rpath");
          args.emplace_back(s);
        }
      }

#ifndef __APPLE__
      // -lm is needed in both compilation modes because it is directly used by
      // the shermes C output.
      args.emplace_back("-lm");
#endif

    } else {
      splitArgs(cfg.ldflags, args);
    }

    if (!params.noHermesLibs) {
      // Either hermesvm_a, hermesvmlean_a, hermesvm, or hermesvmlean.
      std::string libParam = "-lhermesvm";
      if (params.lean == ShermesCompileParams::Lean::on)
        libParam += "lean";
      if (params.staticLink == ShermesCompileParams::StaticLink::on)
        libParam += "_a";
      args.emplace_back(std::move(libParam));
    }

    splitArgs(cfg.ldlibs, args);
  }
  args.emplace_back("-o");
  args.emplace_back(outputPath);

  std::vector<llvh::StringRef> refArgs{};
  refArgs.reserve(args.size());
  for (const auto &str : args)
    refArgs.emplace_back(str);

  if (params.verbosity) {
    for (size_t i = 0; i != refArgs.size(); ++i)
      llvh::errs() << (i ? " " : "") << refArgs[i];
    llvh::errs() << "\n";
  }

  std::string errMsg;
  if (llvh::sys::ExecuteAndWait(
          *res, refArgs, llvh::None, {}, 0, 0, &errMsg, nullptr) == 0) {
    return true;
  }

  if (!errMsg.empty())
    llvh::errs() << errMsg << "\n";
  else
    llvh::errs() << program << ": execution failed\n";
  return false;
}

/// Generate C source, then invoke the C compiler to compile it either to .s,
/// .o, or an executable binary.
bool compileFromC(
    hermes::Context *context,
    hermes::Module &M,
    const ShermesCompileParams &params,
    OutputLevelKind outputLevel,
    llvh::StringRef inputFilename,
    llvh::StringRef outputFilename) {
  // If an output file name is not specified, derive it from the input.
  llvh::SmallString<32> outputPathBuf{};
  if (outputFilename.empty()) {
    if (outputLevel == OutputLevelKind::Executable ||
        outputLevel == OutputLevelKind::SharedObj) {
      outputFilename = "a.out";
    } else {
      assert(
          outputLevel == OutputLevelKind::Asm ||
          outputLevel == OutputLevelKind::Obj);
      outputFilename = deriveFilename(
          inputFilename,
          outputPathBuf,
          outputLevel == OutputLevelKind::Asm ? ".s" : ".o");
    }
  }

  // Synthesize a temporary file name for the .c file. It needs to have the
  // proper extension ".c". Note that createTemporaryFile() automatically
  // appends the ".".
  llvh::SmallString<32> tmpPath;
  int tmpFD = -1;
  if (auto EC = llvh::sys::fs::createTemporaryFile(
          llvh::sys::path::filename(inputFilename), "c", tmpFD, tmpPath)) {
    llvh::errs() << "Error creating " << tmpPath << ": " << EC.message()
                 << '\n';
    return false;
  }
  bool keepTemp = params.keepTemp == ShermesCompileParams::KeepTemp::on;
  // Don't forget to delete the temporary on exit.
  if (!keepTemp) {
    llvh::sys::RemoveFileOnSignal(tmpPath);
  }
  auto removeOnExit = llvh::make_scope_exit([&tmpPath, keepTemp]() {
    // Need this inside the lambda.
    // Putting `make_scope_exit` inside an `if` would be awkward.
    if (!keepTemp) {
      llvh::sys::DontRemoveFileOnSignal(tmpPath);
      ::remove(tmpPath.c_str());
    }
  });

  // Emit into the temporary file.
  {
    llvh::raw_fd_ostream os{tmpFD, true};
    if (!invokeBackend(context, M, params.genOptions, os))
      return false;
    os.close();
    if (auto EC = os.error()) {
      llvh::errs() << "Error writing to " << tmpPath << ": " << EC.message()
                   << '\n';
      return false;
    }
  }

  return invokeCC(params, outputLevel, tmpPath, outputFilename);
}

/// Compile to an executable and run it.
bool execute(
    hermes::Context *context,
    hermes::Module &M,
    const ShermesCompileParams &params,
    llvh::StringRef inputFilename,
    llvh::ArrayRef<std::string> execArgs) {
  llvh::SmallString<32> tmpPath;
  if (auto EC = llvh::sys::fs::createTemporaryFile(
          llvh::sys::path::filename(inputFilename), {}, tmpPath)) {
    llvh::errs() << "Error creating " << tmpPath << ": " << EC.message()
                 << '\n';
    return false;
  }

  bool keepTemp = params.keepTemp == ShermesCompileParams::KeepTemp::on;
  // Don't forget to delete the temporary on exit.
  if (!keepTemp)
    llvh::sys::RemoveFileOnSignal(tmpPath);
  auto removeOnExit = llvh::make_scope_exit([&tmpPath, keepTemp]() {
    if (!keepTemp) {
      llvh::sys::DontRemoveFileOnSignal(tmpPath);
      ::remove(tmpPath.c_str());
    }
  });

  // Produce a shared library that still contains the main function.
  if (!compileFromC(
          context,
          M,
          params,
          OutputLevelKind::SharedObj,
          inputFilename,
          tmpPath)) {
    return false;
  }

  llvh::SmallVector<const char *, 1> args{};
  // Add the library at the start as a dummy argument, since the argument parser
  // will ignore the first argument.
  args.emplace_back(tmpPath.c_str());
  for (auto &s : execArgs)
    args.emplace_back(s.c_str());

  if (params.verbosity) {
    llvh::errs() << "Running library with args:";
    for (size_t i = 0; i != args.size(); ++i)
      llvh::errs() << " " << args[i];
    llvh::errs() << "\n";
  }

  // Open the produced shared library and invoke main with args.
#ifdef _WIN32
  HMODULE handle = LoadLibraryA(tmpPath.c_str());
  if (!handle) {
    llvh::errs() << "LoadLibrary() error, path: " << tmpPath
                 << ", error: " << GetLastError() << "\n";
    return false;
  }
  auto *main = (int (*)(int, char **))GetProcAddress(handle, "main");
  if (!main) {
    llvh::errs() << "GetProcAddress(main) error: " << GetLastError() << "\n";
    return false;
  }
#else
  void *handle = dlopen(tmpPath.c_str(), RTLD_LAZY);
  if (!handle) {
    llvh::errs() << "dlopen() error, path: " << tmpPath
                 << ", error: " << dlerror() << "\n";
    return false;
  }
  auto *main = (int (*)(int, char **))dlsym(handle, "main");
  if (!main) {
    llvh::errs() << "dlsym(main) error: " << dlerror() << "\n";
    return false;
  }
#endif
  // The main function takes a non-const char**, but we know it doesn't actually
  // modify it, so it is harmless to cast.
  return !main(args.size(), const_cast<char **>(args.data()));
}

} // namespace

bool shermesCompile(
    hermes::Context *context,
    hermes::Module &M,
    const ShermesCompileParams &params,
    OutputLevelKind outputLevel,
    llvh::StringRef inputFilename,
    llvh::StringRef outputFilename,
    llvh::ArrayRef<std::string> execArgs) {
  assert(
      outputLevel >= OutputLevelKind::IR &&
      "generateOutput() invoked needlessly");
  if (outputLevel < OutputLevelKind::IR)
    return true;

  if (outputLevel < OutputLevelKind::C) {
    return compileToDump(context, M, params, outputFilename);
  }
  if (outputLevel == OutputLevelKind::C) {
    if (params.genOptions.emitCBundle) {
      return compileToCBundle(
          context, M, params, inputFilename, outputFilename);
    }
    return compileToC(context, M, params, inputFilename, outputFilename);
  }
  if (outputLevel <= OutputLevelKind::Executable) {
    return compileFromC(
        context, M, params, outputLevel, inputFilename, outputFilename);
  }
  if (outputLevel == OutputLevelKind::Run) {
    return execute(context, M, params, inputFilename, execArgs);
  }

  assert(false && "unsupported output level");
  llvh::errs() << "Unsupported compilation mode\n";
  return false;
}
