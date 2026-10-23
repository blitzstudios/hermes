// Links a skeleton bytecode file and separately compiled module bytecode files
// into one bytecode file that runs like a whole-bundle compile.
//
// The skeleton is the bundle with each multi-line module factory replaced by an
// empty function on a single line. Each unit is one of those modules compiled
// on its own. The manifest maps skeleton lines and unit lines back to lines in
// the original bundle:
//   tail <skeleton line> <bundle line>
//   line <skeleton line> <bundle line>
//   unit <path> <skeleton line> <skeleton split column> <bundle start line>
//        <bundle end line> <bundle end column>
// Skeleton positions on a unit's line before the split column belong to the
// module's first line; the rest belong to its last line.
//
// module-cache/compile.js writes the skeleton, units and manifest.

#include "hermes/ADT/StringSetVector.h"
#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/HBC/Bytecode.h"
#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/BCGen/HBC/BytecodeStream.h"
#include "hermes/BCGen/HBC/ConsecutiveStringStorage.h"
#include "hermes/BCGen/HBC/DebugInfo.h"
#include "hermes/BCGen/SerializedLiteralGenerator.h"
#include "hermes/BCGen/ShapeTableEntry.h"
#include "hermes/Inst/Inst.h"
#include "hermes/Regex/RegexSerialization.h"
#include "hermes/SourceMap/SourceMapGenerator.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/Utils/Options.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/StringMap.h"
#include "llvh/Support/CommandLine.h"
#include "llvh/Support/Endian.h"
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/MathExtras.h"
#include "llvh/Support/MemoryBuffer.h"
#include "llvh/Support/SHA1.h"
#include "llvh/Support/raw_ostream.h"

#include <deque>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace hermes;
using namespace hermes::hbc;

namespace {

llvh::cl::opt<std::string> SkeletonFile(
    "skeleton",
    llvh::cl::desc("Bytecode of the skeleton"));
llvh::cl::opt<std::string> ManifestFile(
    "manifest",
    llvh::cl::desc("Unit list and line map"));
llvh::cl::opt<std::string> SourceFile(
    "source",
    llvh::cl::desc("The whole bundle, hashed into the bytecode header"));
llvh::cl::opt<std::string> SourceName(
    "source-name",
    llvh::cl::desc("Filename recorded in debug info (default: -source)"));
llvh::cl::opt<std::string> OutFile("out", llvh::cl::desc("Output bytecode file"));
llvh::cl::opt<bool> OutputSourceMap(
    "output-source-map",
    llvh::cl::desc("Write <out>.map and leave debug info out of <out>"));
llvh::cl::opt<unsigned> Show(
    "show",
    llvh::cl::init(10),
    llvh::cl::desc("Differences to print with -compare"));
llvh::cl::list<std::string> CompareFiles(
    "compare",
    llvh::cl::multi_val(2),
    llvh::cl::desc("Compare the functions of two bytecode files with debug "
                   "info, ignoring encoding and ordering"));

[[noreturn]] void fail(const llvh::Twine &msg) {
  llvh::errs() << "hbc-link: " << msg << "\n";
  exit(1);
}

enum class OT : uint8_t {
  Reg8,
  Reg32,
  UInt8,
  UInt16,
  UInt32,
  Addr8,
  Addr32,
  Imm32,
  Double
};

unsigned otSize(OT t) {
  switch (t) {
    case OT::Reg8:
    case OT::UInt8:
    case OT::Addr8:
      return 1;
    case OT::UInt16:
      return 2;
    case OT::Reg32:
    case OT::UInt32:
    case OT::Addr32:
    case OT::Imm32:
      return 4;
    case OT::Double:
      return 8;
  }
  llvm_unreachable("bad operand type");
}

uint64_t otMax(OT t) {
  return otSize(t) == 8 ? UINT64_MAX : (uint64_t(1) << (8 * otSize(t))) - 1;
}

struct OpDesc {
  const char *name;
  unsigned numOps;
  OT ops[6];
};

const OpDesc kOps[] = {
#define DEFINE_OPCODE_0(name) {#name, 0, {}},
#define DEFINE_OPCODE_1(name, a) {#name, 1, {OT::a}},
#define DEFINE_OPCODE_2(name, a, b) {#name, 2, {OT::a, OT::b}},
#define DEFINE_OPCODE_3(name, a, b, c) {#name, 3, {OT::a, OT::b, OT::c}},
#define DEFINE_OPCODE_4(name, a, b, c, d) \
  {#name, 4, {OT::a, OT::b, OT::c, OT::d}},
#define DEFINE_OPCODE_5(name, a, b, c, d, e) \
  {#name, 5, {OT::a, OT::b, OT::c, OT::d, OT::e}},
#define DEFINE_OPCODE_6(name, a, b, c, d, e, f) \
  {#name, 6, {OT::a, OT::b, OT::c, OT::d, OT::e, OT::f}},
#include "hermes/BCGen/HBC/BytecodeList.def"
};
constexpr unsigned kNumOpcodes = sizeof(kOps) / sizeof(kOps[0]);
static_assert(kNumOpcodes == (unsigned)inst::OpCode::_last);

struct OperandRef {
  const char *op;
  unsigned idx;
};

const OperandRef kAnnotatedIds[] = {
#define OPERAND_STRING_ID(name, n) {#name, n},
#define OPERAND_FUNCTION_ID(name, n) {#name, n},
#define OPERAND_BIGINT_ID(name, n) {#name, n},
#include "hermes/BCGen/HBC/BytecodeList.def"
};

enum class Role : uint8_t {
  None,
  Str,
  Ident,
  Func,
  BigInt,
  RegExp,
  ValBuf,
  Shape,
  StringSwitchIdx,
  TableOffset,
  Jump,
  Unsupported,
};

struct RoleSpec {
  const char *op;
  unsigned idx;
  Role role;
};

const RoleSpec kRoles[] = {
    {"DeclareGlobalVar", 1, Role::Ident},
    {"GetByIdShort", 4, Role::Ident},
    {"GetById", 4, Role::Ident},
    {"GetByIdLong", 4, Role::Ident},
    {"GetByIdWithReceiverLong", 5, Role::Ident},
    {"TryGetById", 4, Role::Ident},
    {"TryGetByIdLong", 4, Role::Ident},
    {"PutByIdLoose", 4, Role::Ident},
    {"PutByIdStrict", 4, Role::Ident},
    {"PutByIdLooseLong", 4, Role::Ident},
    {"PutByIdStrictLong", 4, Role::Ident},
    {"TryPutByIdLoose", 4, Role::Ident},
    {"TryPutByIdStrict", 4, Role::Ident},
    {"TryPutByIdLooseLong", 4, Role::Ident},
    {"TryPutByIdStrictLong", 4, Role::Ident},
    {"DefineOwnById", 4, Role::Ident},
    {"DefineOwnByIdLong", 4, Role::Ident},
    {"CreatePrivateName", 2, Role::Ident},
    {"LoadConstString", 2, Role::Str},
    {"LoadConstStringLongIndex", 2, Role::Str},
    {"CreateRegExp", 2, Role::Str},
    {"CreateRegExp", 3, Role::Str},
    {"CreateRegExp", 4, Role::RegExp},
    {"CreateClosure", 3, Role::Func},
    {"CreateClosureLongIndex", 3, Role::Func},
    {"CreateGenerator", 3, Role::Func},
    {"CreateGeneratorLongIndex", 3, Role::Func},
    {"CreateBaseClass", 4, Role::Func},
    {"CreateBaseClassLongIndex", 4, Role::Func},
    {"CreateDerivedClass", 5, Role::Func},
    {"CreateDerivedClassLongIndex", 5, Role::Func},
    {"LoadConstBigInt", 2, Role::BigInt},
    {"LoadConstBigIntLongIndex", 2, Role::BigInt},
    {"NewObjectWithBuffer", 2, Role::Shape},
    {"NewObjectWithBuffer", 3, Role::ValBuf},
    {"NewObjectWithBufferLong", 2, Role::Shape},
    {"NewObjectWithBufferLong", 3, Role::ValBuf},
    {"NewObjectWithBufferAndParent", 3, Role::Shape},
    {"NewObjectWithBufferAndParent", 4, Role::ValBuf},
    {"NewArrayWithBuffer", 4, Role::ValBuf},
    {"NewArrayWithBufferLong", 4, Role::ValBuf},
    {"CacheNewObject", 3, Role::Shape},
    {"UIntSwitchImm", 2, Role::TableOffset},
    {"StringSwitchImm", 2, Role::StringSwitchIdx},
    {"StringSwitchImm", 3, Role::TableOffset},
    {"CallRequire", 3, Role::Unsupported},
    {"ProfilePoint", 1, Role::Unsupported},
};

// Opcodes that differ only in the width of their ID operands, narrowest first.
const std::vector<std::vector<const char *>> kIdFamilies = {
    {"GetByIdShort", "GetById", "GetByIdLong"},
    {"TryGetById", "TryGetByIdLong"},
    {"PutByIdLoose", "PutByIdLooseLong"},
    {"PutByIdStrict", "PutByIdStrictLong"},
    {"TryPutByIdLoose", "TryPutByIdLooseLong"},
    {"TryPutByIdStrict", "TryPutByIdStrictLong"},
    {"DefineOwnById", "DefineOwnByIdLong"},
    {"LoadConstString", "LoadConstStringLongIndex"},
    {"LoadConstBigInt", "LoadConstBigIntLongIndex"},
    {"CreateClosure", "CreateClosureLongIndex"},
    {"CreateGenerator", "CreateGeneratorLongIndex"},
    {"CreateBaseClass", "CreateBaseClassLongIndex"},
    {"CreateDerivedClass", "CreateDerivedClassLongIndex"},
    {"NewObjectWithBuffer", "NewObjectWithBufferLong"},
    {"NewArrayWithBuffer", "NewArrayWithBufferLong"},
};

struct OpTables {
  llvh::StringMap<unsigned> byName;
  Role roles[kNumOpcodes][6]{};
  std::vector<std::vector<unsigned>> families;
  int family[kNumOpcodes];
  int shortJump[kNumOpcodes];
  int longJump[kNumOpcodes];
  unsigned jumpOperand[kNumOpcodes];
  unsigned size[kNumOpcodes];
  unsigned uintSwitch, stringSwitch, createRegExp;

  unsigned lookup(llvh::StringRef name) const {
    auto it = byName.find(name);
    if (it == byName.end())
      fail("unknown opcode " + name);
    return it->second;
  }

  OpTables() {
    for (unsigned op = 0; op < kNumOpcodes; ++op) {
      byName[kOps[op].name] = op;
      family[op] = shortJump[op] = longJump[op] = -1;
      size[op] = 1;
      for (unsigned k = 0; k < kOps[op].numOps; ++k) {
        size[op] += otSize(kOps[op].ops[k]);
        if (kOps[op].ops[k] == OT::Addr8 || kOps[op].ops[k] == OT::Addr32) {
          roles[op][k] = Role::Jump;
          jumpOperand[op] = k;
        }
      }
    }
    for (const auto &spec : kRoles) {
      unsigned op = lookup(spec.op);
      if (spec.idx < 1 || spec.idx > kOps[op].numOps)
        fail(llvh::Twine("bad operand index for ") + spec.op);
      roles[op][spec.idx - 1] = spec.role;
    }
    for (const auto &ref : kAnnotatedIds) {
      Role r = roles[lookup(ref.op)][ref.idx - 1];
      if (r != Role::Str && r != Role::Ident && r != Role::Func &&
          r != Role::BigInt)
        fail(llvh::Twine("unhandled ID operand ") + ref.op);
    }
    for (const auto &names : kIdFamilies) {
      std::vector<unsigned> ops;
      for (const char *name : names) {
        unsigned op = lookup(name);
        family[op] = families.size();
        ops.push_back(op);
      }
      families.push_back(std::move(ops));
    }
    for (unsigned op = 0; op < kNumOpcodes; ++op) {
      const OpDesc &d = kOps[op];
      for (unsigned k = 0; k < d.numOps; ++k) {
        if (d.ops[k] != OT::Addr8)
          continue;
        auto it = byName.find((llvh::Twine(d.name) + "Long").str());
        if (it == byName.end())
          fail(llvh::Twine("no long variant for ") + d.name);
        longJump[op] = it->second;
        shortJump[it->second] = op;
      }
    }
    uintSwitch = lookup("UIntSwitchImm");
    stringSwitch = lookup("StringSwitchImm");
    createRegExp = lookup("CreateRegExp");
  }
};

const OpTables &ops() {
  static OpTables tables;
  return tables;
}

uint64_t readOperand(const uint8_t *p, OT t) {
  using namespace llvh::support;
  switch (t) {
    case OT::Reg8:
    case OT::UInt8:
      return *p;
    case OT::Addr8:
      return uint64_t(int64_t(int8_t(*p)));
    case OT::UInt16:
      return endian::read<uint16_t, little, 1>(p);
    case OT::Reg32:
    case OT::UInt32:
      return endian::read<uint32_t, little, 1>(p);
    case OT::Addr32:
    case OT::Imm32:
      return uint64_t(int64_t(endian::read<int32_t, little, 1>(p)));
    case OT::Double:
      return endian::read<uint64_t, little, 1>(p);
  }
  llvm_unreachable("bad operand type");
}

void writeOperand(std::vector<uint8_t> &out, OT t, uint64_t v) {
  for (unsigned i = 0, n = otSize(t); i < n; ++i)
    out.push_back(uint8_t(v >> (8 * i)));
}

const uint8_t *align4(const uint8_t *p) {
  return reinterpret_cast<const uint8_t *>(
      (reinterpret_cast<uintptr_t>(p) + 3) & ~uintptr_t(3));
}

struct Lit {
  SerializedLiteralGenerator::TagType tag;
  uint64_t value = 0;
};

using LitList = std::vector<Lit>;

void parseLiterals(
    llvh::ArrayRef<uint8_t> buf,
    uint32_t offset,
    unsigned count,
    LitList &out) {
  using SLG = SerializedLiteralGenerator;
  using namespace llvh::support;
  size_t i = offset;
  while (count) {
    if (i >= buf.size())
      fail("literal buffer overrun");
    uint8_t tag = buf[i++];
    size_t inSeq = tag & 0x0f;
    if (tag & 0x80)
      inSeq = (inSeq << 8) | buf[i++];
    size_t n = std::min<size_t>(count, inSeq);
    count -= n;
    SLG::TagType type = tag & SLG::TagMask;
    for (size_t j = 0; j < n; ++j) {
      Lit lit{type};
      switch (type) {
        case SLG::ShortStringTag:
          lit.value = endian::read<uint16_t, little, 1>(&buf[i]);
          i += 2;
          break;
        case SLG::LongStringTag:
        case SLG::IntegerTag:
          lit.value = endian::read<uint32_t, little, 1>(&buf[i]);
          i += 4;
          break;
        case SLG::NumberTag:
          lit.value = endian::read<uint64_t, little, 1>(&buf[i]);
          i += 8;
          break;
        default:
          break;
      }
      out.push_back(lit);
    }
  }
}

bool isStringLit(const Lit &lit) {
  return lit.tag == SerializedLiteralGenerator::ShortStringTag ||
      lit.tag == SerializedLiteralGenerator::LongStringTag;
}

// Mirrors SerializedLiteralGenerator::serializeBuffer, with string values
// already resolved to IDs in the linked string table.
void serializeLiterals(const LitList &lits, std::vector<uint8_t> &buff) {
  using SLG = SerializedLiteralGenerator;
  auto appendTag = [&buff](SLG::TagType tag, size_t seqLength) {
    if (seqLength > 15) {
      buff.push_back((tag | 0x80) | (seqLength >> 8));
      buff.push_back(seqLength & 0xff);
    } else {
      buff.push_back(tag + seqLength);
    }
  };
  SLG::TagType lastTag = SLG::NumberTag;
  std::vector<uint8_t> seq;
  size_t seqLength = 0;
  for (const Lit &lit : lits) {
    SLG::TagType tag = lit.tag;
    if (isStringLit(lit))
      tag = lit.value > UINT16_MAX ? SLG::LongStringTag : SLG::ShortStringTag;
    if (tag != lastTag || seqLength == SLG::SequenceMax) {
      if (seqLength > 0) {
        appendTag(lastTag, seqLength);
        buff.insert(buff.end(), seq.begin(), seq.end());
        seq.clear();
      }
      lastTag = tag;
      seqLength = 0;
    }
    seqLength++;
    switch (tag) {
      case SLG::ShortStringTag:
        writeOperand(seq, OT::UInt16, lit.value);
        break;
      case SLG::LongStringTag:
      case SLG::IntegerTag:
        writeOperand(seq, OT::UInt32, lit.value);
        break;
      case SLG::NumberTag:
        writeOperand(seq, OT::Double, lit.value);
        break;
      default:
        break;
    }
  }
  appendTag(lastTag, seqLength);
  buff.insert(buff.end(), seq.begin(), seq.end());
}

struct Insn {
  uint8_t op;
  uint64_t v[6]{};
  int32_t table = -1;
  int32_t keys = -1;
  int32_t values = -1;
};

struct SwitchTable {
  bool isString;
  uint32_t insn;
  std::vector<uint32_t> labels;
  std::vector<uint32_t> targets;
};

struct DebugLoc {
  uint32_t insn;
  uint32_t line, column, statement;
};

struct Func {
  unsigned src;
  uint32_t fid;
  std::vector<Insn> insns;
  std::vector<SwitchTable> tables;
  std::vector<LitList> lits;
  std::vector<HBCExceptionHandlerInfo> handlers;
  bool hasDebug = false;
  DebugSourceLocation start;
  std::vector<DebugLoc> locs;
};

struct Unit {
  std::string path;
  uint32_t skelLine, skelSplitCol, bundleStart, bundleEnd, bundleEndCol;
};

struct Source {
  std::unique_ptr<llvh::MemoryBuffer> file;
  std::unique_ptr<BCProviderFromBuffer> bc;
  std::vector<std::string> strings;
  int unit = -1;
  std::vector<uint32_t> newId;

  void load(const std::string &path) {
    auto fileOrErr = llvh::MemoryBuffer::getFile(path);
    if (!fileOrErr)
      fail("cannot read " + path);
    file = std::move(*fileOrErr);
    auto ret = BCProviderFromBuffer::createBCProviderFromBuffer(
        std::make_unique<hermes::MemoryBuffer>(file.get()));
    if (!ret.first)
      fail(path + ": " + ret.second);
    bc = std::move(ret.first);
    strings.resize(bc->getStringCount());
    std::string tmp;
    for (uint32_t i = 0, e = bc->getStringCount(); i < e; ++i)
      strings[i] = getUTF8StringFromEntry(
                       bc->getStringTableEntry(i), bc->getStringStorage(), tmp)
                       .str();
    newId.assign(bc->getFunctionCount(), UINT32_MAX);
  }

  const std::string &str(uint64_t id) const {
    if (id >= strings.size())
      fail("string ID out of range");
    return strings[id];
  }

  OptValue<DebugSourceLocation> functionStart(uint32_t fid) const {
    const DebugOffsets *offsets = bc->getDebugOffsets(fid);
    if (!offsets || offsets->sourceLocations == DebugOffsets::NO_OFFSET)
      return llvh::None;
    hbc::detail::FunctionDebugInfoDeserializer fdid(
        bc->getDebugInfo()->viewData().getData(), offsets->sourceLocations);
    return fdid.getCurrent();
  }
};

Func decodeFunction(const Source &S, unsigned srcIdx, uint32_t fid) {
  const OpTables &T = ops();
  Func F;
  F.src = srcIdx;
  F.fid = fid;
  auto header = S.bc->getFunctionHeader(fid);
  uint32_t size = header.getBytecodeSizeInBytes();
  const uint8_t *bc = S.bc->getBytecode(fid);

  std::vector<int32_t> posToInsn(size + 1, -1);
  std::vector<uint32_t> positions;
  for (uint32_t pos = 0; pos < size;) {
    if (bc[pos] >= kNumOpcodes)
      fail("bad opcode");
    posToInsn[pos] = positions.size();
    positions.push_back(pos);
    pos += T.size[bc[pos]];
    if (pos > size)
      fail("instruction overruns function");
  }
  posToInsn[size] = positions.size();
  auto insnAt = [&](int64_t pos) -> uint32_t {
    if (pos < 0 || pos > size || posToInsn[pos] < 0)
      fail("branch to the middle of an instruction");
    return posToInsn[pos];
  };

  F.insns.reserve(positions.size());
  for (uint32_t i = 0; i < positions.size(); ++i) {
    uint32_t pos = positions[i];
    Insn I;
    I.op = bc[pos];
    const OpDesc &d = kOps[I.op];
    const uint8_t *p = bc + pos + 1;
    for (unsigned k = 0; k < d.numOps; ++k) {
      I.v[k] = readOperand(p, d.ops[k]);
      p += otSize(d.ops[k]);
      switch (T.roles[I.op][k]) {
        case Role::Jump:
          I.v[k] = insnAt(int64_t(pos) + int64_t(I.v[k]));
          break;
        case Role::Unsupported:
          fail(llvh::Twine("unsupported opcode ") + d.name);
        default:
          break;
      }
    }
    if (I.op == T.uintSwitch || I.op == T.stringSwitch) {
      using namespace llvh::support;
      SwitchTable tab;
      tab.isString = I.op == T.stringSwitch;
      tab.insn = i;
      const uint8_t *table = align4(bc + pos + I.v[tab.isString ? 2 : 1]);
      uint64_t count = tab.isString ? I.v[4] : I.v[4] - I.v[3] + 1;
      for (uint64_t j = 0; j < count; ++j) {
        if (tab.isString) {
          tab.labels.push_back(endian::read<uint32_t, little, 1>(table));
          table += 4;
        }
        tab.targets.push_back(
            insnAt(int64_t(pos) + endian::read<int32_t, little, 1>(table)));
        table += 4;
      }
      I.table = F.tables.size();
      F.tables.push_back(std::move(tab));
    }
    F.insns.push_back(I);
  }

  for (const auto &eh : S.bc->getExceptionTable(fid))
    F.handlers.push_back(
        {insnAt(eh.start), insnAt(eh.end), insnAt(eh.target)});

  const DebugOffsets *offsets = S.bc->getDebugOffsets(fid);
  if (offsets && offsets->sourceLocations != DebugOffsets::NO_OFFSET) {
    F.hasDebug = true;
    hbc::detail::FunctionDebugInfoDeserializer fdid(
        S.bc->getDebugInfo()->viewData().getData(), offsets->sourceLocations);
    if (fdid.getFunctionIndex() != fid)
      fail("debug info belongs to another function");
    F.start = fdid.getCurrent();
    for (;;) {
      auto loc = fdid.next();
      if (fdid.isDone())
        break;
      const DebugSourceLocation &cur = fdid.getCurrent();
      if (cur.envIdx != 0)
        fail("scoping debug info is not supported");
      if (loc)
        F.locs.push_back(
            {insnAt(cur.address), loc->line, loc->column, loc->statement});
      else
        F.locs.push_back({insnAt(cur.address), 0, 0, 0});
    }
  }

  auto shapeAt = [&](uint64_t idx) {
    auto shapes = S.bc->getObjectShapeTable();
    if (idx >= shapes.size())
      fail("shape index out of range");
    return shapes[idx];
  };
  for (Insn &I : F.insns) {
    int shapeK = -1, valK = -1;
    for (unsigned k = 0; k < kOps[I.op].numOps; ++k) {
      if (T.roles[I.op][k] == Role::Shape)
        shapeK = k;
      else if (T.roles[I.op][k] == Role::ValBuf)
        valK = k;
    }
    if (shapeK < 0 && valK < 0)
      continue;
    unsigned count;
    if (shapeK >= 0) {
      ShapeTableEntry shape = shapeAt(I.v[shapeK]);
      count = shape.numProps;
      I.keys = F.lits.size();
      F.lits.emplace_back();
      parseLiterals(
          S.bc->getObjectKeyBuffer(),
          shape.keyBufferOffset,
          count,
          F.lits.back());
    } else {
      count = I.v[2];
    }
    if (valK >= 0) {
      I.values = F.lits.size();
      F.lits.emplace_back();
      parseLiterals(
          S.bc->getLiteralValueBuffer(), I.v[valK], count, F.lits.back());
    }
  }
  return F;
}

struct Manifest {
  uint32_t tailSkel = 0, tailBundle = 0;
  std::vector<Unit> units;
  std::map<uint32_t, uint32_t> lines;
  llvh::DenseMap<uint32_t, unsigned> unitBySkelLine;
  uint32_t firstUnitLine = UINT32_MAX;

  void load(const std::string &path) {
    auto fileOrErr = llvh::MemoryBuffer::getFile(path);
    if (!fileOrErr)
      fail("cannot read " + path);
    std::istringstream in((*fileOrErr)->getBuffer().str());
    std::string kind;
    while (in >> kind) {
      if (kind == "tail") {
        in >> tailSkel >> tailBundle;
      } else if (kind == "line") {
        uint32_t skel, bundle;
        in >> skel >> bundle;
        lines[skel] = bundle;
      } else if (kind == "unit") {
        Unit u;
        in >> u.path >> u.skelLine >> u.skelSplitCol >> u.bundleStart >>
            u.bundleEnd >> u.bundleEndCol;
        unitBySkelLine[u.skelLine] = units.size();
        firstUnitLine = std::min(firstUnitLine, u.skelLine);
        units.push_back(std::move(u));
      } else {
        fail("bad manifest entry " + kind);
      }
      if (!in)
        fail("truncated manifest");
    }
    if (!tailSkel)
      fail("manifest has no tail");
  }

  void mapSkeleton(uint32_t &line, uint32_t &column) const {
    if (line == 0)
      return;
    auto u = unitBySkelLine.find(line);
    if (u != unitBySkelLine.end()) {
      const Unit &unit = units[u->second];
      if (column < unit.skelSplitCol) {
        line = unit.bundleStart;
      } else {
        line = unit.bundleEnd;
        column = column - unit.skelSplitCol + unit.bundleEndCol;
      }
      return;
    }
    auto l = lines.find(line);
    if (l != lines.end()) {
      line = l->second;
      return;
    }
    if (line >= tailSkel) {
      line = line - tailSkel + tailBundle;
      return;
    }
    if (line >= firstUnitLine)
      fail("skeleton line " + llvh::Twine(line) + " is not in the manifest");
  }
};

std::string functionKey(const Source &S, uint32_t fid) {
  std::string key = S.str(S.bc->getFunctionHeader(fid).getFunctionName());
  if (auto start = S.functionStart(fid))
    key += "@" + std::to_string(start->line) + ":" +
        std::to_string(start->column);
  return key;
}

void printLits(
    llvh::raw_ostream &os,
    const Source &S,
    const LitList &lits) {
  using SLG = SerializedLiteralGenerator;
  os << "[";
  for (const Lit &lit : lits) {
    if (isStringLit(lit))
      os << '"' << S.str(lit.value) << '"';
    else if (lit.tag == SLG::IntegerTag)
      os << int32_t(lit.value);
    else if (lit.tag == SLG::NumberTag)
      os << "d" << lit.value;
    else
      os << "t" << unsigned(lit.tag);
    os << ",";
  }
  os << "]";
}

std::string canonicalFunction(const Source &S, uint32_t fid) {
  const OpTables &T = ops();
  Func F = decodeFunction(S, 0, fid);
  std::string out;
  llvh::raw_string_ostream os(out);
  auto h = S.bc->getFunctionHeader(fid);
  auto flags = h.getFlags();
  os << "params " << h.getParamCount() << " frame " << h.getFrameSize()
     << " num " << h.getNumberRegCount() << " nonptr "
     << h.getNonPtrRegCount() << " read " << unsigned(h.getReadCacheSize())
     << " write " << unsigned(h.getWriteCacheSize()) << " cachenew "
     << unsigned(h.getNumCacheNewObject()) << " private "
     << unsigned(h.getPrivateNameCacheSize()) << " loop " << h.getLoopDepth()
     << " invoke " << unsigned(flags.getProhibitInvoke()) << " strict "
     << flags.getStrictMode() << " kind " << unsigned(flags.getKind())
     << "\n";
  for (size_t i = 0; i < F.insns.size(); ++i) {
    const Insn &I = F.insns[i];
    int fam = T.family[I.op];
    unsigned op = I.op;
    if (fam >= 0)
      op = T.families[fam][0];
    else if (T.shortJump[op] >= 0)
      op = T.shortJump[op];
    os << i << " " << kOps[op].name;
    for (unsigned k = 0; k < kOps[I.op].numOps; ++k) {
      os << " ";
      uint64_t v = I.v[k];
      switch (T.roles[I.op][k]) {
        case Role::Str:
        case Role::Ident:
          os << '"' << S.str(v) << '"';
          break;
        case Role::Func:
          os << "F(" << functionKey(S, v) << ")";
          break;
        case Role::Shape:
          printLits(os, S, F.lits[I.keys]);
          break;
        case Role::ValBuf:
          printLits(os, S, F.lits[I.values]);
          break;
        case Role::RegExp:
          os << "re";
          break;
        case Role::StringSwitchIdx:
          os << "ss";
          break;
        case Role::TableOffset: {
          const SwitchTable &tab = F.tables[I.table];
          os << "{";
          for (size_t j = 0; j < tab.targets.size(); ++j) {
            if (tab.isString)
              os << '"' << S.str(tab.labels[j]) << "\":";
            os << "@" << tab.targets[j] << ",";
          }
          os << "}";
          break;
        }
        case Role::Jump:
          os << "@" << v;
          break;
        default:
          os << v;
          break;
      }
    }
    os << "\n";
  }
  for (const auto &eh : F.handlers)
    os << "eh " << eh.start << " " << eh.end << " " << eh.target << "\n";
  if (F.hasDebug) {
    os << "start " << F.start.line << ":" << F.start.column << "\n";
    for (const DebugLoc &l : F.locs)
      os << "loc " << l.insn << " " << l.line << ":" << l.column << " "
         << l.statement << "\n";
  }
  return os.str();
}

int compareMain() {
  std::deque<Source> files(2);
  std::map<std::string, std::vector<std::string>> funcs[2];
  std::vector<std::string> keys[2];
  for (int i = 0; i < 2; ++i) {
    files[i].load(CompareFiles[i]);
    for (uint32_t fid = 0; fid < files[i].bc->getFunctionCount(); ++fid) {
      keys[i].push_back(functionKey(files[i], fid));
      funcs[i][keys[i].back()].push_back(canonicalFunction(files[i], fid));
    }
  }
  unsigned samePlace = 0;
  for (size_t fid = 0; fid < std::min(keys[0].size(), keys[1].size()); ++fid) {
    if (keys[0][fid] == keys[1][fid])
      ++samePlace;
    else if (fid - samePlace < Show)
      llvh::outs() << "index " << fid << ": " << keys[0][fid] << " vs "
                   << keys[1][fid] << "\n";
  }
  llvh::outs() << samePlace << " of " << keys[0].size()
               << " functions have the same index\n";
  unsigned same = 0, differ = 0, missing = 0, shown = 0;
  for (int i = 0; i < 2; ++i)
    for (auto &[key, list] : funcs[i])
      std::sort(list.begin(), list.end());
  for (auto &[key, a] : funcs[0]) {
    auto it = funcs[1].find(key);
    if (it == funcs[1].end()) {
      missing += a.size();
      if (shown++ < Show)
        llvh::outs() << "only in " << CompareFiles[0] << ": " << key << "\n";
      continue;
    }
    auto &b = it->second;
    for (size_t j = 0; j < std::max(a.size(), b.size()); ++j) {
      if (j < a.size() && j < b.size() && a[j] == b[j]) {
        ++same;
        continue;
      }
      ++differ;
      if (shown++ >= Show)
        continue;
      llvh::outs() << "differs: " << key << "\n";
      std::istringstream sa(j < a.size() ? a[j] : ""),
          sb(j < b.size() ? b[j] : "");
      std::string la, lb;
      unsigned printed = 0;
      while (printed < 6) {
        bool ga = (bool)std::getline(sa, la), gb = (bool)std::getline(sb, lb);
        if (!ga && !gb)
          break;
        if (!ga)
          la.clear();
        if (!gb)
          lb.clear();
        if (la != lb) {
          llvh::outs() << "  - " << la << "\n  + " << lb << "\n";
          ++printed;
        }
      }
    }
  }
  for (auto &[key, b] : funcs[1])
    if (!funcs[0].count(key)) {
      missing += b.size();
      if (shown++ < Show)
        llvh::outs() << "only in " << CompareFiles[1] << ": " << key << "\n";
    }
  llvh::outs() << same << " identical, " << differ << " differ, " << missing
               << " unmatched\n";
  return differ || missing ? 1 : 0;
}

int linkMain() {
  const OpTables &T = ops();
  if (SkeletonFile.empty() || ManifestFile.empty() || SourceFile.empty() ||
      OutFile.empty())
    fail("-skeleton, -manifest, -source and -out are required");
  Manifest manifest;
  manifest.load(ManifestFile);
  std::string sourceName = SourceName.empty() ? SourceFile : SourceName;

  std::deque<Source> sources(1 + manifest.units.size());
  sources[0].load(SkeletonFile);
  for (unsigned u = 0; u < manifest.units.size(); ++u) {
    sources[u + 1].load(manifest.units[u].path);
    sources[u + 1].unit = u;
    if (sources[u + 1].bc->getBytecodeOptions()._flags !=
        sources[0].bc->getBytecodeOptions()._flags)
      fail(manifest.units[u].path + " was compiled with different options");
  }
  for (const Source &S : sources)
    if (!S.bc->getCJSModuleTable().empty() ||
        !S.bc->getCJSModuleTableStatic().empty())
      fail("CommonJS module tables are not supported");

  {
    const llvh::StringRef prefix = kTemplateObjectIDPrefix;
    std::map<uint32_t, llvh::StringRef> templates;
    for (const Source &S : sources)
      for (const std::string &str : S.strings) {
        llvh::StringRef entry = str;
        if (!entry.consume_front(prefix))
          continue;
        auto [idText, raw] = entry.split('\0');
        uint32_t id;
        if (idText.getAsInteger(10, id))
          fail("malformed template object entry");
        auto [it, inserted] = templates.insert({id, raw});
        if (!inserted && it->second != raw)
          fail("template object ID collision across modules");
      }
  }
  Source &skel = sources[0];

  // Stubs are the only functions on their skeleton lines.
  std::vector<int> stubUnit(skel.bc->getFunctionCount(), -1);
  std::vector<int> unitStub(manifest.units.size(), -1);
  for (uint32_t fid = 0; fid < skel.bc->getFunctionCount(); ++fid) {
    if (fid == skel.bc->getGlobalFunctionIndex())
      continue;
    auto start = skel.functionStart(fid);
    if (!start)
      continue;
    auto it = manifest.unitBySkelLine.find(start->line);
    if (it == manifest.unitBySkelLine.end())
      continue;
    if (unitStub[it->second] >= 0)
      fail("two functions on the stub line of " +
           manifest.units[it->second].path);
    unitStub[it->second] = fid;
    stubUnit[fid] = it->second;
  }
  for (unsigned u = 0; u < manifest.units.size(); ++u)
    if (unitStub[u] < 0)
      fail("no stub for " + manifest.units[u].path);

  // A unit's factory is the one closure its global function creates.
  std::vector<uint32_t> factory(manifest.units.size());
  for (unsigned u = 0; u < manifest.units.size(); ++u) {
    const Source &S = sources[u + 1];
    Func G = decodeFunction(S, u + 1, S.bc->getGlobalFunctionIndex());
    int found = -1;
    for (const Insn &I : G.insns)
      for (unsigned k = 0; k < kOps[I.op].numOps; ++k)
        if (T.roles[I.op][k] == Role::Func) {
          if (found >= 0)
            fail(manifest.units[u].path + " creates more than one closure");
          found = I.v[k];
        }
    if (found < 0)
      fail(manifest.units[u].path + " creates no closure");
    factory[u] = found;
  }

  std::vector<std::pair<unsigned, uint32_t>> order;
  for (uint32_t fid = 0; fid < skel.bc->getFunctionCount(); ++fid) {
    int u = stubUnit[fid];
    if (u < 0) {
      skel.newId[fid] = order.size();
      order.push_back({0, fid});
      continue;
    }
    Source &S = sources[u + 1];
    skel.newId[fid] = S.newId[factory[u]] = order.size();
    order.push_back({u + 1, factory[u]});
    for (uint32_t f = 0; f < S.bc->getFunctionCount(); ++f) {
      if (f == factory[u] || f == S.bc->getGlobalFunctionIndex())
        continue;
      S.newId[f] = order.size();
      order.push_back({u + 1, f});
    }
  }

  std::vector<Func> funcs;
  funcs.reserve(order.size());
  for (auto [src, fid] : order)
    funcs.push_back(decodeFunction(sources[src], src, fid));

  BytecodeModule BM(order.size());
  StringLiteralTable &strings = BM.getStringLiteralTableMut();
  for (const Func &F : funcs) {
    const Source &S = sources[F.src];
    for (const Insn &I : F.insns)
      for (unsigned k = 0; k < kOps[I.op].numOps; ++k) {
        Role r = T.roles[I.op][k];
        if (r == Role::Str || r == Role::Ident)
          strings.addString(S.str(I.v[k]), r == Role::Ident);
        else if (r == Role::BigInt)
          fail("BigInt literals are not supported");
      }
    for (const Insn &I : F.insns) {
      if (I.keys >= 0)
        for (const Lit &lit : F.lits[I.keys])
          if (isStringLit(lit))
            strings.addString(S.str(lit.value), true);
      if (I.values >= 0)
        for (const Lit &lit : F.lits[I.values])
          if (isStringLit(lit))
            strings.addString(S.str(lit.value), false);
    }
    for (const SwitchTable &tab : F.tables)
      for (uint32_t label : tab.labels)
        strings.addString(S.str(label), false);
    strings.addString(
        S.str(S.bc->getFunctionHeader(F.fid).getFunctionName()), false);
  }
  std::vector<std::pair<uint32_t, const std::string *>> functionSources;
  for (const Source &S : sources)
    for (auto [fid, stringID] : S.bc->getFunctionSourceTable()) {
      if (S.newId[fid] == UINT32_MAX)
        continue;
      strings.addString(S.str(stringID), false);
      functionSources.push_back({S.newId[fid], &S.str(stringID)});
    }
  strings.populateStorage(StringLiteralTable::OptimizeMode::ReorderAndPack);
  BM.populateStringMetadataFromStringTable();
  std::sort(functionSources.begin(), functionSources.end());
  for (auto [fid, source] : functionSources)
    BM.addFunctionSource(fid, BM.getStringID(*source));

  // Mirrors LiteralBufferBuilder: values and keys are uniqued and packed, and
  // shapes are numbered objects first, then CacheNewObject.
  {
    StringSetVector valSet, keySet;
    struct Use {
      Func *F;
      Insn *I;
      uint32_t keyIdx = UINT32_MAX, valIdx = UINT32_MAX;
    };
    std::vector<Use> uses;
    std::vector<uint8_t> tmp;
    auto resolved = [&](const Source &S, LitList lits, bool isKey) {
      for (Lit &lit : lits)
        if (isStringLit(lit))
          lit.value = isKey ? BM.getIdentifierID(S.str(lit.value))
                            : BM.getStringID(S.str(lit.value));
      tmp.clear();
      serializeLiterals(lits, tmp);
      return llvh::StringRef((const char *)tmp.data(), tmp.size());
    };
    for (Func &F : funcs) {
      const Source &S = sources[F.src];
      for (Insn &I : F.insns) {
        if (I.keys < 0 && I.values < 0)
          continue;
        Use use{&F, &I};
        if (I.keys >= 0)
          use.keyIdx = keySet.insert(resolved(S, F.lits[I.keys], true));
        if (I.values >= 0)
          use.valIdx = valSet.insert(resolved(S, F.lits[I.values], false));
        uses.push_back(use);
      }
    }
    const StringSetVector &vals = valSet, &keys = keySet;
    ConsecutiveStringStorage valStorage{
        vals.begin(), vals.end(), std::true_type{}, true};
    ConsecutiveStringStorage keyStorage{
        keys.begin(), keys.end(), std::true_type{}, true};
    auto valView = static_cast<const ConsecutiveStringStorage &>(valStorage)
                       .getStringTableView();
    auto keyView = static_cast<const ConsecutiveStringStorage &>(keyStorage)
                       .getStringTableView();
    std::vector<ShapeTableEntry> shapes;
    llvh::DenseMap<std::pair<uint32_t, uint32_t>, uint32_t> shapeIdx;
    auto assign = [&](Use &use) {
      Insn &I = *use.I;
      for (unsigned k = 0; k < kOps[I.op].numOps; ++k) {
        Role r = T.roles[I.op][k];
        if (r == Role::ValBuf) {
          I.v[k] = valView[use.valIdx].getOffset();
        } else if (r == Role::Shape) {
          uint32_t offset = keyView[use.keyIdx].getOffset();
          uint32_t count = use.F->lits[I.keys].size();
          auto [it, inserted] =
              shapeIdx.insert({{offset, count}, (uint32_t)shapes.size()});
          if (inserted)
            shapes.push_back({offset, count});
          I.v[k] = it->second;
        }
      }
    };
    for (Use &use : uses)
      if (use.I->values >= 0)
        assign(use);
    for (Use &use : uses)
      if (use.I->values < 0)
        assign(use);
    BM.initializeSerializedLiterals(
        std::move(valStorage).acquireStringTableAndStorage().second,
        std::move(keyStorage).acquireStringTableAndStorage().second,
        std::move(shapes));
  }

  std::deque<CompiledRegExp> regexps;
  std::map<std::pair<std::string, std::string>, uint32_t> regexpIds;
  for (Func &F : funcs) {
    const Source &S = sources[F.src];
    for (Insn &I : F.insns) {
      if (I.op == T.createRegExp) {
        auto oldEntry = S.bc->getRegExpTable()[I.v[3]];
        auto oldBytecode = S.bc->getRegExpStorage().slice(
            oldEntry.offset, oldEntry.length);
        const std::string &pattern = S.str(I.v[1]);
        const std::string &flags = S.str(I.v[2]);
        auto [it, inserted] =
            regexpIds.insert({{pattern, flags}, (uint32_t)regexps.size()});
        if (inserted) {
          llvh::StringRef error;
          auto re = CompiledRegExp::tryCompile(pattern, flags, &error);
          if (!re)
            fail("regexp does not compile: " + error);
          regexps.push_back(std::move(*re));
          it->second = BM.addRegExp(&regexps.back());
        }
        auto newEntry = BM.getRegExpTable()[it->second];
        if (!BM.getRegExpStorage()
                 .slice(newEntry.offset, newEntry.length)
                 .equals(oldBytecode))
          fail("recompiled regexp differs from the unit's");
        I.v[3] = it->second;
      }
      for (unsigned k = 0; k < kOps[I.op].numOps; ++k) {
        switch (T.roles[I.op][k]) {
          case Role::Str:
            I.v[k] = BM.getStringID(S.str(I.v[k]));
            break;
          case Role::Ident:
            I.v[k] = BM.getIdentifierID(S.str(I.v[k]));
            break;
          case Role::Func:
            if (I.v[k] >= S.newId.size() || S.newId[I.v[k]] == UINT32_MAX)
              fail("closure over a function that was not linked");
            I.v[k] = S.newId[I.v[k]];
            break;
          case Role::StringSwitchIdx:
            I.v[k] = BM.addStringSwitchImmInstr();
            break;
          default:
            break;
        }
      }
    }
    for (SwitchTable &tab : F.tables)
      for (uint32_t &label : tab.labels)
        label = BM.getStringID(S.str(label));
  }

  DebugInfoGenerator debugGen(BM.getDebugInfo());
  uint32_t filenameId = debugGen.addFilename(sourceName);

  for (uint32_t newFid = 0; newFid < funcs.size(); ++newFid) {
    Func &F = funcs[newFid];
    const Source &S = sources[F.src];
    const size_t n = F.insns.size();

    for (Insn &I : F.insns) {
      int fam = T.family[I.op];
      if (fam >= 0) {
        for (unsigned candidate : T.families[fam]) {
          bool fits = true;
          for (unsigned k = 0; k < kOps[candidate].numOps; ++k) {
            Role r = T.roles[candidate][k];
            if (r != Role::None && r != Role::Jump &&
                I.v[k] > otMax(kOps[candidate].ops[k]))
              fits = false;
          }
          if (fits) {
            I.op = candidate;
            break;
          }
        }
      }
      for (unsigned k = 0; k < kOps[I.op].numOps; ++k) {
        Role r = T.roles[I.op][k];
        if (r != Role::None && r != Role::Jump && r != Role::TableOffset &&
            I.v[k] > otMax(kOps[I.op].ops[k]))
          fail(llvh::Twine("operand does not fit in ") + kOps[I.op].name);
      }
      if (T.longJump[I.op] >= 0)
        I.op = T.longJump[I.op];
    }

    // Mirrors HBCISel::resolveRelocations: every jump starts long and passes
    // over the function shrink the ones that fit, seeing updated positions
    // behind them and the previous pass's positions ahead.
    std::vector<int64_t> loc(n + 1);
    for (size_t i = 0; i < n; ++i)
      loc[i + 1] = loc[i] + T.size[F.insns[i].op];
    for (bool changed = true; changed;) {
      changed = false;
      int64_t shift = 0;
      for (size_t i = 0; i < n; ++i) {
        loc[i] -= shift;
        Insn &I = F.insns[i];
        if (T.shortJump[I.op] < 0)
          continue;
        int64_t offset = loc[I.v[T.jumpOperand[I.op]]] - loc[i];
        if (-128 <= offset && offset < 128) {
          I.op = T.shortJump[I.op];
          shift += 3;
          changed = true;
        }
      }
      loc[n] -= shift;
    }
    for (size_t i = 0; i < n; ++i)
      loc[i + 1] = loc[i] + T.size[F.insns[i].op];
    const uint32_t bytecodeSize = loc[n];

    uint32_t uintTableBytes = 0;
    for (const SwitchTable &tab : F.tables)
      if (!tab.isString)
        uintTableBytes += tab.targets.size() * 4;
    std::vector<uint32_t> tableOffset(F.tables.size());
    {
      uint32_t uintOff = 0, strOff = uintTableBytes;
      for (size_t t = 0; t < F.tables.size(); ++t) {
        const SwitchTable &tab = F.tables[t];
        tableOffset[t] = tab.isString ? strOff : uintOff;
        (tab.isString ? strOff : uintOff) +=
            tab.targets.size() * (tab.isString ? 8 : 4);
      }
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(bytecodeSize);
    for (size_t i = 0; i < n; ++i) {
      const Insn &I = F.insns[i];
      const OpDesc &d = kOps[I.op];
      bytes.push_back(I.op);
      for (unsigned k = 0; k < d.numOps; ++k) {
        uint64_t v = I.v[k];
        switch (T.roles[I.op][k]) {
          case Role::Jump: {
            int64_t offset = loc[v] - loc[i];
            if (d.ops[k] == OT::Addr8 && (offset < -128 || offset >= 128))
              fail("short jump out of range");
            v = uint64_t(offset);
            break;
          }
          case Role::TableOffset:
            v = bytecodeSize + tableOffset[I.table] - loc[i];
            break;
          default:
            break;
        }
        writeOperand(bytes, d.ops[k], v);
      }
    }
    if (!F.tables.empty()) {
      bytes.resize(llvh::alignTo(bytes.size(), 4), 0);
      for (bool strings : {false, true})
        for (const SwitchTable &tab : F.tables) {
          if (tab.isString != strings)
            continue;
          int64_t base = loc[tab.insn];
          for (size_t j = 0; j < tab.targets.size(); ++j) {
            if (strings)
              writeOperand(bytes, OT::UInt32, tab.labels[j]);
            writeOperand(
                bytes, OT::UInt32, uint64_t(loc[tab.targets[j]] - base));
          }
        }
    }

    auto oldHeader = S.bc->getFunctionHeader(F.fid);
    FunctionHeader header{
        bytecodeSize,
        oldHeader.getParamCount(),
        oldHeader.getLoopDepth(),
        oldHeader.getFrameSize(),
        oldHeader.getNumberRegCount(),
        oldHeader.getNonPtrRegCount(),
        BM.getStringID(S.str(oldHeader.getFunctionName())),
        (uint8_t)oldHeader.getReadCacheSize(),
        (uint8_t)oldHeader.getWriteCacheSize(),
        (uint8_t)oldHeader.getNumCacheNewObject(),
        (uint8_t)oldHeader.getPrivateNameCacheSize()};
    header.flags = oldHeader.getFlags();
    header.flags.setOverflowed(false);
    header.flags.setHasDebugInfo(false);
    header.flags.setHasExceptionHandler(!F.handlers.empty());

    std::vector<HBCExceptionHandlerInfo> handlers;
    for (const auto &eh : F.handlers)
      handlers.push_back({(uint32_t)loc[eh.start],
                          (uint32_t)loc[eh.end],
                          (uint32_t)loc[eh.target]});

    auto bcFunc = std::make_unique<BytecodeFunction>(
        std::move(bytes), std::move(header), std::move(handlers));

    if (F.hasDebug) {
      const int unit = S.unit;
      auto mapPos = [&](uint32_t &line, uint32_t &column) {
        if (line == 0)
          return;
        if (unit < 0)
          manifest.mapSkeleton(line, column);
        else
          line += manifest.units[unit].bundleStart - 1;
      };
      DebugSourceLocation start = F.start;
      mapPos(start.line, start.column);
      start.filenameId = filenameId;
      start.sourceMappingUrlId = kInvalidSourceMappingUrlId;
      start.address = 0;
      start.statement = 0;
      start.envIdx = 0;
      std::vector<DebugSourceLocation> locs;
      locs.reserve(F.locs.size());
      for (const DebugLoc &l : F.locs) {
        uint32_t line = l.line, column = l.column;
        mapPos(line, column);
        locs.emplace_back(
            (uint32_t)loc[l.insn], filenameId, line, column, l.statement);
      }
      bcFunc->setDebugOffsets(
          {debugGen.appendSourceLocations(start, newFid, locs)});
    }
    BM.setFunction(newFid, std::move(bcFunc));
  }
  std::move(debugGen).generate();

  BM.setGlobalFunctionIndex(skel.newId[skel.bc->getGlobalFunctionIndex()]);
  BM.getBytecodeOptionsMut() = skel.bc->getBytecodeOptions();
  BM.setSegmentID(skel.bc->getSegmentID());

  auto sourceOrErr = llvh::MemoryBuffer::getFile(SourceFile);
  if (!sourceOrErr)
    fail("cannot read " + SourceFile);
  llvh::SHA1 hasher;
  hasher.update((*sourceOrErr)->getBuffer());
  auto rawHash = hasher.final();
  SHA1 sourceHash{};
  std::copy(rawHash.begin(), rawHash.end(), sourceHash.begin());

  BytecodeGenerationOptions genOptions = BytecodeGenerationOptions::defaults();
  genOptions.format = EmitBundle;
  genOptions.optimizationEnabled = true;
  genOptions.stripDebugInfoSection = OutputSourceMap;

  std::error_code ec;
  llvh::raw_fd_ostream os(OutFile, ec, llvh::sys::fs::F_None);
  if (ec)
    fail("cannot write " + OutFile + ": " + ec.message());
  serializeBytecodeModule(BM, sourceHash, os, genOptions);
  os.close();

  if (OutputSourceMap) {
    SourceMapGenerator sourceMap;
    sourceMap.addSource(sourceName);
    BM.populateSourceMap(&sourceMap);
    llvh::raw_fd_ostream mapOS(OutFile + ".map", ec, llvh::sys::fs::F_Text);
    if (ec)
      fail("cannot write " + OutFile + ".map: " + ec.message());
    sourceMap.outputAsJSON(mapOS);
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  llvh::cl::ParseCommandLineOptions(argc, argv, "Hermes bytecode linker\n");
  return CompareFiles.empty() ? linkMain() : compareMain();
}
