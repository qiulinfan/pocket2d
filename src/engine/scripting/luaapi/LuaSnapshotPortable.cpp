#include "LuaSnapshot.h"

#include "lua.hpp"

#include <cstdint>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The portable state for desync recovery (LuaSnapshot::Export / Import).
//
//   "PKS1", object count
//   per object, in the order they were reached:
//     kind, (opaque: its Lua type), anchor step and its operands, (closure: proto index)
//   protos: count, per proto the hash of its stripped dump and its shape
//     (first and last line, upvalues, parameters); the code itself is sent
//     only when the importing side does not have it (Dumps)
//   per table:   metatable value, pair count, key, value ...
//   per closure: upvalue count, value ...
//
// Objects are numbered breadth first from the roots; an object's anchor is the
// first path that reached it: a root, a key of a table (a scalar key, or a key
// object numbered before it), a metatable, or a closure's upvalue. Objects only
// reachable as keys of tables have no anchor. Values are nil, booleans,
// integers, floats, strings (each string once, then by number) and references.
// A pair whose key is an engine value (userdata, a C function: the bindings'
// class registry keys) belongs to the process: it is neither exported nor
// cleared by an import.
namespace LuaSnapshot {
namespace {

enum Kind : std::uint8_t { kTable = 1, kFunction = 2, kStatic = 3, kLocal = 4, kOpaque = 5 };
enum Step : std::uint8_t { kRoot = 0, kKey = 1, kObjectKey = 2, kMeta = 3, kUpvalue = 4, kNoAnchor = 5 };
enum Tag : std::uint8_t { kNil = 0, kFalse = 1, kTrue = 2, kInt = 3, kFloat = 4, kString = 5, kStringRef = 6, kRef = 7 };
constexpr char kMagic[4] = {'P', 'K', 'S', '1'};

struct Entry {
    std::uint8_t kind = 0;
    std::uint8_t type = 0; // opaque values: the Lua type
    std::uint8_t step = kNoAnchor;
    std::uint32_t parent = 0;
    std::uint32_t index = 0; // root number, key object, upvalue number
    std::uint32_t proto = 0;
};

std::uint8_t Classify(lua_State *L, int index) {
    switch (lua_type(L, index)) {
    case LUA_TTABLE: {
        const void *p = lua_topointer(L, index);
        if (detail::IsStatic(p)) return kStatic;
        if (detail::IsLocal(p)) return kLocal;
        return kTable;
    }
    case LUA_TFUNCTION: return lua_iscfunction(L, index) ? kOpaque : kFunction;
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
    case LUA_TTHREAD: return kOpaque;
    default: return 0;
    }
}

// Identity of an object value (the type keeps a light userdata apart from a table at the same address).
bool ProcessKey(lua_State *L, int index) {
    const int t = lua_type(L, index);
    return t == LUA_TLIGHTUSERDATA || t == LUA_TUSERDATA || t == LUA_TTHREAD || (t == LUA_TFUNCTION && lua_iscfunction(L, index));
}

std::uintptr_t Identity(lua_State *L, int index) {
    return reinterpret_cast<std::uintptr_t>(lua_topointer(L, index)) * 8u + static_cast<std::uintptr_t>(lua_type(L, index));
}

int DumpWriter(lua_State *, const void *p, std::size_t size, void *ud) {
    static_cast<std::string *>(ud)->append(static_cast<const char *>(p), size);
    return 0;
}

std::uint64_t Fnv64(const std::string &bytes) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : bytes) h = (h ^ c) * 1099511628211ULL;
    return h;
}

// The identity of a Lua function's code: its stripped dump (equal code behaves the same).
// A Lua function's shape: its first and last line, upvalues and parameters
// (a cheap filter before the hash).
std::array<int, 4> Shape(lua_State *L, int index) {
    lua_Debug ar;
    lua_pushvalue(L, index);
    lua_getinfo(L, ">Su", &ar);
    return {ar.linedefined, ar.lastlinedefined, ar.nups, ar.nparams};
}

std::uint64_t ProtoHash(lua_State *L, int index) {
    std::string dump;
    lua_pushvalue(L, index);
    lua_dump(L, DumpWriter, &dump, 1);
    lua_pop(L, 1);
    return Fnv64(dump);
}

struct Writer {
    std::string out;
    std::unordered_map<std::string, std::uint32_t> strings;
    void Byte(std::uint8_t b) { out.push_back(static_cast<char>(b)); }
    void U(std::uint64_t v) {
        while (v >= 0x80) {
            Byte(static_cast<std::uint8_t>(v | 0x80));
            v >>= 7;
        }
        Byte(static_cast<std::uint8_t>(v));
    }
    void Raw(const void *p, std::size_t n) { out.append(static_cast<const char *>(p), n); }
};

struct Reader {
    const std::string &in;
    std::size_t at = 0;
    bool bad = false;
    std::uint8_t Byte() {
        if (at >= in.size()) { bad = true; return 0; }
        return static_cast<std::uint8_t>(in[at++]);
    }
    std::uint64_t U() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const std::uint8_t b = Byte();
            if (bad) return 0;
            v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        bad = true;
        return 0;
    }
    bool Raw(void *p, std::size_t n) {
        if (at > in.size() || in.size() - at < n) { bad = true; return false; }
        std::memcpy(p, in.data() + at, n);
        at += n;
        return true;
    }
};

struct Value {
    std::uint8_t tag = kNil;
    std::int64_t i = 0;
    double d = 0;
    std::uint32_t ref = 0; // a string's number or an object's
};

std::uint64_t ZigZag(std::int64_t v) { return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63); }
std::int64_t UnZigZag(std::uint64_t v) { return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1); }

class Exporter {
public:
    explicit Exporter(lua_State *L) : L(L) {}

    std::string Run() {
        lua_checkstack(L, 32);
        // Entries of weak tables whose keys are otherwise unreachable go first.
        lua_gc(L, LUA_GCCOLLECT);
        const int top = lua_gettop(L);
        lua_newtable(L);
        objects_ = lua_gettop(L);
        lua_newtable(L);
        keys_ = lua_gettop(L);
        lua_newtable(L);
        pending_ = lua_gettop(L);

        const int roots = detail::PushRoots();
        const int R = lua_gettop(L);
        for (int r = 1; r <= roots; ++r) {
            lua_rawgeti(L, R, r);
            Reach(-1, Entry{0, 0, kRoot, 0, static_cast<std::uint32_t>(r), 0});
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        std::size_t next = 0;
        for (;;) {
            while (next < entries_.size()) Expand(static_cast<std::uint32_t>(next++));
            if (!ResolvePending()) break;
        }
        Write();
        lua_settop(L, top);
        return std::move(w_.out);
    }

private:
    bool Known(int index, std::uint32_t *id = nullptr) const {
        const auto found = ids_.find(Identity(L, index));
        if (found == ids_.end()) return false;
        if (id) *id = found->second;
        return true;
    }

    // Numbers the object at `index` (if it is one and new) with the given anchor.
    void Reach(int index, Entry anchor, std::uint8_t as = 0) {
        const std::uint8_t kind = as ? as : Classify(L, index);
        if (!kind || Known(index)) return;
        index = lua_absindex(L, index);
        const auto id = static_cast<std::uint32_t>(entries_.size());
        anchor.kind = kind;
        anchor.type = static_cast<std::uint8_t>(lua_type(L, index));
        if (kind == kFunction) anchor.proto = Proto(index);
        entries_.push_back(anchor);
        ids_[Identity(L, index)] = id;
        lua_pushvalue(L, index);
        lua_rawseti(L, objects_, id + 1);
    }

    std::uint32_t Proto(int index) {
        const std::uint64_t hash = ProtoHash(L, index);
        const auto found = protos_.find(hash);
        if (found != protos_.end()) return found->second;
        std::string dump;
        lua_pushvalue(L, index);
        lua_dump(L, DumpWriter, &dump, 0);
        lua_pop(L, 1);
        const auto n = static_cast<std::uint32_t>(proto_list_.size());
        proto_list_.push_back({hash, Shape(L, index)});
        if (dumps_) (*dumps_)[hash] = std::move(dump);
        protos_[hash] = n;
        return n;
    }

    void Expand(std::uint32_t id) {
        const std::uint8_t kind = entries_[id].kind;
        if (kind != kTable && kind != kFunction) return;
        lua_rawgeti(L, objects_, id + 1);
        const int obj = lua_gettop(L);
        if (kind == kTable) {
            if (lua_getmetatable(L, obj)) {
                Reach(-1, Entry{0, 0, kMeta, id, 0, 0});
                lua_pop(L, 1);
            }
            lua_pushnil(L);
            while (lua_next(L, obj)) {
                if (ProcessKey(L, -2)) {
                    // left out
                } else if (Classify(L, -2)) {
                    // An object key: its value's path needs the key numbered first.
                    const int n = pending_count_;
                    lua_pushinteger(L, id);
                    lua_rawseti(L, pending_, n * 3 + 1);
                    lua_pushvalue(L, -2);
                    lua_rawseti(L, pending_, n * 3 + 2);
                    lua_pushvalue(L, -1);
                    lua_rawseti(L, pending_, n * 3 + 3);
                    ++pending_count_;
                } else if (Classify(L, -1) && !Known(-1)) {
                    const auto vid = static_cast<std::uint32_t>(entries_.size());
                    Reach(-1, Entry{0, 0, kKey, id, 0, 0});
                    lua_pushvalue(L, -2);
                    lua_rawseti(L, keys_, vid + 1);
                }
                lua_pop(L, 1);
            }
        } else {
            for (int u = 1; lua_getupvalue(L, obj, u); ++u) {
                Reach(-1, Entry{0, 0, kUpvalue, id, static_cast<std::uint32_t>(u), 0});
                lua_pop(L, 1);
            }
        }
        lua_settop(L, obj - 1);
    }

    // Pairs with object keys: values whose key is numbered get that path; when
    // none can, the first unnumbered key is numbered without a path. False when
    // nothing is left.
    bool ResolvePending() {
        bool progress = false;
        int first_unknown = -1, kept = 0;
        for (int n = 0; n < pending_count_; ++n) {
            lua_rawgeti(L, pending_, n * 3 + 1);
            const auto parent = static_cast<std::uint32_t>(lua_tointeger(L, -1));
            lua_rawgeti(L, pending_, n * 3 + 2);
            lua_rawgeti(L, pending_, n * 3 + 3);
            std::uint32_t key_id = 0;
            if (Known(-2, &key_id)) {
                if (Classify(L, -1) && !Known(-1)) {
                    Reach(-1, Entry{0, 0, kObjectKey, parent, key_id, 0});
                    progress = true;
                }
            } else {
                if (first_unknown < 0) first_unknown = kept;
                for (int k = 0; k < 3; ++k) {
                    lua_pushvalue(L, -3 + k);
                    lua_rawseti(L, pending_, kept * 3 + 1 + k);
                }
                ++kept;
            }
            lua_pop(L, 3);
        }
        for (int k = kept * 3 + 1; k <= pending_count_ * 3; ++k) {
            lua_pushnil(L);
            lua_rawseti(L, pending_, k);
        }
        pending_count_ = kept;
        if (progress) return true;
        if (first_unknown < 0) return false;
        if (FindInStatics()) return true;
        lua_rawgeti(L, pending_, first_unknown * 3 + 2);
        Reach(-1, Entry{0, 0, kNoAnchor, 0, 0, 0});
        lua_pop(L, 1);
        return true;
    }

    // Key objects without a path may be parts of static data (only the marked
    // table is static, its parts are never walked): searched breadth first from
    // the static tables reached, and numbered as static by their path there.
    // True when one was found.
    bool FindInStatics() {
        if (searched_statics_) return false;
        searched_statics_ = true;
        lua_checkstack(L, 32);
        lua_newtable(L);
        const int wanted = lua_gettop(L); // key object -> true
        int nwanted = 0;
        for (int n = 0; n < pending_count_; ++n) {
            lua_rawgeti(L, pending_, n * 3 + 2);
            if (lua_istable(L, -1) && !Known(-1)) {
                lua_pushboolean(L, 1);
                lua_rawset(L, wanted);
                ++nwanted;
            } else {
                lua_pop(L, 1);
            }
        }
        lua_newtable(L);
        const int parent_of = lua_gettop(L); // table -> its parent table
        lua_newtable(L);
        const int key_of = lua_gettop(L);    // table -> its key there
        lua_newtable(L);
        const int queue = lua_gettop(L);
        int head = 0, tail = 0;
        for (std::uint32_t id = 0; id < entries_.size(); ++id) {
            if (entries_[id].kind != kStatic) continue;
            lua_rawgeti(L, objects_, id + 1);
            lua_rawseti(L, queue, ++tail);
        }
        int found = 0;
        while (head < tail && found < nwanted) {
            lua_rawgeti(L, queue, ++head);
            const int t = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, t)) {
                if (lua_istable(L, -1) && !Classify(L, -2) && !Known(-1)) {
                    lua_pushvalue(L, -1);
                    if (lua_rawget(L, parent_of) == LUA_TNIL) {
                        lua_pop(L, 1);
                        lua_pushvalue(L, -1);
                        lua_pushvalue(L, t);
                        lua_rawset(L, parent_of);
                        lua_pushvalue(L, -1);
                        lua_pushvalue(L, -3);
                        lua_rawset(L, key_of);
                        lua_pushvalue(L, -1);
                        lua_rawseti(L, queue, ++tail);
                        lua_pushvalue(L, -1);
                        if (lua_rawget(L, wanted) != LUA_TNIL) {
                            ++found;
                            ChainToStatic(lua_gettop(L) - 1, parent_of, key_of);
                        }
                        lua_pop(L, 1);
                    } else {
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 4);
        return found > 0;
    }

    // Numbers the table at `index` and the parts above it (up to a numbered static) as static.
    void ChainToStatic(int index, int parent_of, int key_of) {
        index = lua_absindex(L, index);
        if (Known(index) || !lua_istable(L, index)) return;
        lua_checkstack(L, 8);
        lua_pushvalue(L, index);
        lua_rawget(L, parent_of);
        ChainToStatic(lua_gettop(L), parent_of, key_of);
        std::uint32_t parent = 0;
        Known(-1, &parent);
        lua_pop(L, 1);
        const auto id = static_cast<std::uint32_t>(entries_.size());
        Reach(index, Entry{0, 0, kKey, parent, 0, 0}, kStatic);
        lua_pushvalue(L, index);
        lua_rawget(L, key_of);
        lua_rawseti(L, keys_, id + 1);
    }

    void Put(int index) {
        switch (lua_type(L, index)) {
        case LUA_TBOOLEAN: w_.Byte(lua_toboolean(L, index) ? kTrue : kFalse); return;
        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) {
                w_.Byte(kInt);
                w_.U(ZigZag(lua_tointeger(L, index)));
            } else {
                const double d = lua_tonumber(L, index);
                std::uint64_t bits;
                std::memcpy(&bits, &d, sizeof bits);
                w_.Byte(kFloat);
                for (int b = 0; b < 8; ++b) w_.Byte(static_cast<std::uint8_t>(bits >> (8 * b)));
            }
            return;
        case LUA_TSTRING: {
            std::size_t n = 0;
            const char *p = lua_tolstring(L, index, &n);
            std::string text(p, n);
            const auto found = w_.strings.find(text);
            if (found != w_.strings.end()) {
                w_.Byte(kStringRef);
                w_.U(found->second);
            } else {
                w_.Byte(kString);
                w_.U(n);
                w_.Raw(p, n);
                const auto number = static_cast<std::uint32_t>(w_.strings.size());
                w_.strings.emplace(std::move(text), number);
            }
            return;
        }
        default: {
            std::uint32_t id = 0;
            if (Classify(L, index) && Known(index, &id)) {
                w_.Byte(kRef);
                w_.U(id);
            } else {
                w_.Byte(kNil);
            }
            return;
        }
        }
    }

    void Write() {
        w_.Raw(kMagic, 4);
        w_.U(entries_.size());
        for (std::uint32_t id = 0; id < entries_.size(); ++id) {
            const Entry &e = entries_[id];
            w_.Byte(e.kind);
            if (e.kind == kOpaque) w_.Byte(e.type);
            w_.Byte(e.step);
            switch (e.step) {
            case kRoot: w_.U(e.index); break;
            case kKey:
                w_.U(e.parent);
                lua_rawgeti(L, keys_, id + 1);
                Put(-1);
                lua_pop(L, 1);
                break;
            case kObjectKey: w_.U(e.parent); w_.U(e.index); break;
            case kMeta: w_.U(e.parent); break;
            case kUpvalue: w_.U(e.parent); w_.U(e.index); break;
            default: break;
            }
            if (e.kind == kFunction) w_.U(e.proto);
        }
        w_.U(proto_list_.size());
        for (const auto &[hash, shape] : proto_list_) {
            for (int b = 0; b < 8; ++b) w_.Byte(static_cast<std::uint8_t>(hash >> (8 * b)));
            for (const int v : shape) w_.U(static_cast<std::uint64_t>(v));
        }
        for (std::uint32_t id = 0; id < entries_.size(); ++id) {
            const std::uint8_t kind = entries_[id].kind;
            if (kind != kTable && kind != kFunction) continue;
            lua_rawgeti(L, objects_, id + 1);
            const int obj = lua_gettop(L);
            if (kind == kTable) {
                if (!lua_getmetatable(L, obj)) lua_pushnil(L);
                Put(-1);
                lua_pop(L, 1);
                std::uint64_t pairs = 0;
                lua_pushnil(L);
                while (lua_next(L, obj)) {
                    if (!ProcessKey(L, -2)) ++pairs;
                    lua_pop(L, 1);
                }
                w_.U(pairs);
                lua_pushnil(L);
                while (lua_next(L, obj)) {
                    if (!ProcessKey(L, -2)) {
                        Put(-2);
                        Put(-1);
                    }
                    lua_pop(L, 1);
                }
            } else {
                int n = 0;
                while (lua_getupvalue(L, obj, n + 1)) {
                    lua_pop(L, 1);
                    ++n;
                }
                w_.U(static_cast<std::uint64_t>(n));
                for (int u = 1; u <= n; ++u) {
                    lua_getupvalue(L, obj, u);
                    Put(-1);
                    lua_pop(L, 1);
                }
            }
            lua_settop(L, obj - 1);
        }
    }

    lua_State *L;
    int objects_ = 0, keys_ = 0, pending_ = 0;
    int pending_count_ = 0;
    bool searched_statics_ = false;
    std::vector<Entry> entries_;
    std::unordered_map<std::uintptr_t, std::uint32_t> ids_;
    std::unordered_map<std::uint64_t, std::uint32_t> protos_;
    std::vector<std::pair<std::uint64_t, std::array<int, 4>>> proto_list_;
    Writer w_;

public:
    Dumps *dumps_ = nullptr;
};

class Importer {
public:
    Importer(lua_State *L, const std::string &data) : L(L), r_{data} {}

    bool Run(std::string &error, ImportStats *stats) {
        if (!Parse()) { error = "the state data is damaged"; return false; }
        lua_checkstack(L, 32);
        const int top = lua_gettop(L);
        lua_createtable(L, static_cast<int>(entries_.size()), 0);
        values_ = lua_gettop(L);
        // Find or make every object first; nothing is written until all are there.
        for (std::uint32_t id = 0; id < entries_.size(); ++id) {
            if (!Resolve(id, error)) { lua_settop(L, top); return false; }
        }
        if (!missing_.empty()) {
            error = "code missing here: " + std::to_string(missing_.size()) + " functions";
            lua_settop(L, top);
            return false;
        }
        if (check_only_) { lua_settop(L, top); return true; }
        for (std::uint32_t id = 0; id < entries_.size(); ++id) Fill(id);
        lua_settop(L, top);
        if (stats) *stats = stats_;
        return true;
    }

private:
    bool ReadValue(Value &v) {
        v.tag = r_.Byte();
        switch (v.tag) {
        case kNil:
        case kFalse:
        case kTrue: return !r_.bad;
        case kInt: v.i = UnZigZag(r_.U()); return !r_.bad;
        case kFloat: {
            std::uint64_t bits = 0;
            for (int b = 0; b < 8; ++b) bits |= static_cast<std::uint64_t>(r_.Byte()) << (8 * b);
            std::memcpy(&v.d, &bits, sizeof v.d);
            return !r_.bad;
        }
        case kString: {
            const std::uint64_t n = r_.U();
            if (r_.bad || n > r_.in.size()) return false;
            std::string text(static_cast<std::size_t>(n), '\0');
            if (!r_.Raw(text.data(), text.size())) return false;
            v.tag = kStringRef;
            v.ref = static_cast<std::uint32_t>(strings_.size());
            strings_.push_back(std::move(text));
            return true;
        }
        case kStringRef:
            v.ref = static_cast<std::uint32_t>(r_.U());
            return !r_.bad && v.ref < strings_.size();
        case kRef:
            v.ref = static_cast<std::uint32_t>(r_.U());
            return !r_.bad && v.ref < entries_.size();
        default: return false;
        }
    }

    bool Parse() {
        char magic[4];
        if (!r_.Raw(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return false;
        const std::uint64_t n = r_.U();
        if (r_.bad || n > r_.in.size()) return false;
        entries_.resize(static_cast<std::size_t>(n));
        key_values_.resize(static_cast<std::size_t>(n));
        for (std::uint32_t id = 0; id < n; ++id) {
            Entry &e = entries_[id];
            e.kind = r_.Byte();
            if (e.kind == kOpaque) e.type = r_.Byte();
            e.step = r_.Byte();
            switch (e.step) {
            case kRoot: e.index = static_cast<std::uint32_t>(r_.U()); break;
            case kKey:
                e.parent = static_cast<std::uint32_t>(r_.U());
                if (!ReadValue(key_values_[id])) return false;
                break;
            case kObjectKey:
                e.parent = static_cast<std::uint32_t>(r_.U());
                e.index = static_cast<std::uint32_t>(r_.U());
                if (e.index >= id) return false;
                break;
            case kMeta: e.parent = static_cast<std::uint32_t>(r_.U()); break;
            case kUpvalue:
                e.parent = static_cast<std::uint32_t>(r_.U());
                e.index = static_cast<std::uint32_t>(r_.U());
                break;
            case kNoAnchor: break;
            default: return false;
            }
            if (e.step != kRoot && e.step != kNoAnchor && e.parent >= id) return false;
            if (e.kind == kFunction) e.proto = static_cast<std::uint32_t>(r_.U());
            if (r_.bad || e.kind < kTable || e.kind > kOpaque) return false;
        }
        const std::uint64_t protos = r_.U();
        if (r_.bad || protos > r_.in.size()) return false;
        for (std::uint64_t p = 0; p < protos; ++p) {
            std::uint64_t hash = 0;
            for (int b = 0; b < 8; ++b) hash |= static_cast<std::uint64_t>(r_.Byte()) << (8 * b);
            std::array<int, 4> shape{};
            for (int &v : shape) v = static_cast<int>(r_.U());
            if (r_.bad) return false;
            protos_.emplace_back(hash, shape);
        }
        contents_.resize(entries_.size());
        for (std::uint32_t id = 0; id < entries_.size(); ++id) {
            const Entry &e = entries_[id];
            if (e.kind == kFunction && e.proto >= protos_.size()) return false;
            if (e.kind != kTable && e.kind != kFunction) continue;
            auto &c = contents_[id];
            if (e.kind == kTable) {
                c.emplace_back();
                if (!ReadValue(c.back())) return false; // metatable
            }
            const std::uint64_t count = r_.U();
            if (r_.bad || count > r_.in.size()) return false;
            const std::uint64_t values = e.kind == kTable ? count * 2 : count;
            for (std::uint64_t k = 0; k < values; ++k) {
                c.emplace_back();
                if (!ReadValue(c.back())) return false;
            }
        }
        return !r_.bad && r_.at == r_.in.size();
    }

    void Push(const Value &v) {
        switch (v.tag) {
        case kFalse: lua_pushboolean(L, 0); break;
        case kTrue: lua_pushboolean(L, 1); break;
        case kInt: lua_pushinteger(L, static_cast<lua_Integer>(v.i)); break;
        case kFloat: lua_pushnumber(L, v.d); break;
        case kStringRef: lua_pushlstring(L, strings_[v.ref].data(), strings_[v.ref].size()); break;
        case kRef: lua_rawgeti(L, values_, v.ref + 1); break;
        default: lua_pushnil(L); break;
        }
    }

    // Pushes the object on this machine at the entry's path (or nil).
    void PushAnchored(const Entry &e) {
        if (e.step == kRoot) {
            detail::PushRoots();
            lua_rawgeti(L, -1, e.index);
            lua_remove(L, -2);
            return;
        }
        if (e.step == kNoAnchor) { lua_pushnil(L); return; }
        lua_rawgeti(L, values_, e.parent + 1);
        const int parent = lua_gettop(L);
        switch (e.step) {
        case kKey:
            if (lua_istable(L, parent)) {
                Push(key_values_[&e - entries_.data()]);
                lua_rawget(L, parent);
            } else {
                lua_pushnil(L);
            }
            break;
        case kObjectKey:
            lua_rawgeti(L, values_, e.index + 1);
            if (lua_istable(L, parent) && !lua_isnil(L, -1)) lua_rawget(L, parent);
            else { lua_pop(L, 1); lua_pushnil(L); }
            break;
        case kMeta:
            if (!lua_istable(L, parent) || !lua_getmetatable(L, parent)) lua_pushnil(L);
            break;
        case kUpvalue:
            if (lua_type(L, parent) != LUA_TFUNCTION || lua_iscfunction(L, parent) ||
                !lua_getupvalue(L, parent, static_cast<int>(e.index)))
                lua_pushnil(L);
            break;
        default: lua_pushnil(L); break;
        }
        lua_remove(L, parent);
    }

    bool Resolve(std::uint32_t id, std::string &error) {
        const Entry &e = entries_[id];
        PushAnchored(e);
        const int c = lua_gettop(L);
        const void *p = lua_topointer(L, c);
        bool keep = false;
        switch (e.kind) {
        case kTable:
            keep = lua_istable(L, c) && !detail::IsStatic(p) && !detail::IsLocal(p) && used_.insert(p).second;
            if (!keep) {
                lua_pop(L, 1);
                lua_newtable(L);
                ++stats_.created;
            }
            ++stats_.objects;
            break;
        case kFunction:
            keep = lua_type(L, c) == LUA_TFUNCTION && !lua_iscfunction(L, c) && !used_.count(p) &&
                   ProtoHash(L, c) == protos_[e.proto].first;
            if (keep) {
                used_.insert(p);
            } else {
                lua_pop(L, 1);
                if (!NewClosure(e.proto, error)) return false;
                ++stats_.created;
            }
            ++stats_.objects;
            break;
        case kStatic: // never written: any table at the path (parts of static data are not marked)
            keep = lua_istable(L, c);
            break;
        case kLocal:
            keep = lua_istable(L, c);
            break;
        default:
            keep = lua_type(L, c) == e.type && !lua_isnil(L, c) && (e.type != LUA_TFUNCTION || lua_iscfunction(L, c));
            break;
        }
        if (!keep && e.kind != kTable && e.kind != kFunction) {
            lua_pop(L, 1);
            lua_pushnil(L);
            ++stats_.unresolved;
        }
        lua_rawseti(L, values_, id + 1);
        return true;
    }

    void Fill(std::uint32_t id) {
        const Entry &e = entries_[id];
        if (e.kind != kTable && e.kind != kFunction) return;
        const auto &c = contents_[id];
        lua_rawgeti(L, values_, id + 1);
        const int obj = lua_gettop(L);
        if (e.kind == kTable) {
            // Cleared and filled again in the host's order.
            lua_newtable(L);
            const int keys = lua_gettop(L);
            int nkeys = 0;
            lua_pushnil(L);
            while (lua_next(L, obj)) {
                lua_pop(L, 1);
                if (ProcessKey(L, -1)) continue;
                lua_pushvalue(L, -1);
                lua_rawseti(L, keys, ++nkeys);
            }
            for (int k = 1; k <= nkeys; ++k) {
                lua_rawgeti(L, keys, k);
                lua_pushnil(L);
                lua_rawset(L, obj);
            }
            lua_pop(L, 1);
            for (std::size_t k = 1; k + 1 < c.size(); k += 2) {
                Push(c[k]);
                if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; } // a key not found here
                Push(c[k + 1]);
                lua_rawset(L, obj);
            }
            // Only a changed metatable is set (as LuaSnapshot::Load).
            Push(c[0]);
            if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); }
            if (!lua_getmetatable(L, obj)) lua_pushnil(L);
            const bool same = lua_rawequal(L, -1, -2);
            lua_pop(L, 1);
            if (same) lua_pop(L, 1);
            else lua_setmetatable(L, obj);
        } else {
            for (std::size_t u = 0; u < c.size(); ++u) {
                Push(c[u]);
                if (!lua_setupvalue(L, obj, static_cast<int>(u + 1))) lua_pop(L, 1);
            }
        }
        lua_settop(L, obj - 1);
    }

    // Every Lua function reachable from the registry whose shape one of the
    // state's protos has, by the hash of its code (to make closures from).
    void IndexFunctions() {
        indexed_ = true;
        std::unordered_set<std::uint64_t> shapes;
        auto shape_key = [](const std::array<int, 4> &s) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s[0])) << 32) ^
                   (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s[1])) << 12) ^
                   (static_cast<std::uint64_t>(s[2]) << 6) ^ static_cast<std::uint64_t>(s[3]);
        };
        for (const auto &[hash, shape] : protos_) shapes.insert(shape_key(shape));
        lua_checkstack(L, 16);
        lua_newtable(L);
        index_ = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_rawgeti(L, LUA_REGISTRYINDEX, index_);
        const int index = lua_gettop(L);
        lua_newtable(L);
        const int queue = lua_gettop(L);
        std::unordered_set<const void *> seen;
        int head = 0, tail = 0;
        lua_pushvalue(L, LUA_REGISTRYINDEX);
        lua_rawseti(L, queue, ++tail);
        auto add = [&](int at) {
            const int t = lua_type(L, at);
            if (t != LUA_TTABLE && (t != LUA_TFUNCTION || lua_iscfunction(L, at))) return;
            if (!seen.insert(lua_topointer(L, at)).second) return;
            lua_pushvalue(L, at);
            lua_rawseti(L, queue, ++tail);
        };
        while (head < tail) {
            lua_rawgeti(L, queue, ++head);
            lua_pushnil(L);
            lua_rawseti(L, queue, head);
            const int o = lua_gettop(L);
            if (lua_istable(L, o)) {
                if (lua_getmetatable(L, o)) { add(-1); lua_pop(L, 1); }
                lua_pushnil(L);
                while (lua_next(L, o)) {
                    add(-2);
                    add(-1);
                    lua_pop(L, 1);
                }
            } else {
                for (int u = 1; lua_getupvalue(L, o, u); ++u) {
                    add(-1);
                    lua_pop(L, 1);
                }
                if (shapes.count(shape_key(Shape(L, o)))) {
                    const std::uint64_t h = ProtoHash(L, o);
                    lua_pushinteger(L, static_cast<lua_Integer>(h));
                    if (lua_rawget(L, index) == LUA_TNIL) {
                        lua_pop(L, 1);
                        lua_pushinteger(L, static_cast<lua_Integer>(h));
                        lua_pushvalue(L, o);
                        lua_rawset(L, index);
                    } else {
                        lua_pop(L, 1);
                    }
                }
            }
            lua_settop(L, o - 1);
        }
        lua_pop(L, 2);
    }

    // The code of a proto: from a function of this process with the same code, or from the exporter's dumps.
    bool FindDump(std::uint32_t proto, std::string &dump) {
        if (!indexed_) IndexFunctions();
        const std::uint64_t hash = protos_[proto].first;
        lua_rawgeti(L, LUA_REGISTRYINDEX, index_);
        lua_pushinteger(L, static_cast<lua_Integer>(hash));
        const bool here = lua_rawget(L, -2) == LUA_TFUNCTION;
        if (here) lua_dump(L, DumpWriter, &dump, 0);
        lua_pop(L, 2);
        if (here) return true;
        if (dumps_) {
            const auto found = dumps_->find(hash);
            if (found != dumps_->end()) { dump = found->second; return true; }
        }
        return false;
    }

    bool NewClosure(std::uint32_t proto, std::string &error) {
        std::string dump;
        if (!FindDump(proto, dump)) {
            missing_.push_back(protos_[proto].first);
            lua_pushnil(L); // stands in while only the missing code is asked for
            return true;
        }
        if (luaL_loadbufferx(L, dump.data(), dump.size(), "=resync", "b") != LUA_OK) {
            error = std::string("a function of the state does not load: ") + lua_tostring(L, -1);
            return false;
        }
        return true;
    }

public:
    const Dumps *dumps_ = nullptr;
    bool check_only_ = false;
    std::vector<std::uint64_t> missing_;
    ~Importer() {
        if (index_ != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, index_);
    }

private:
    lua_State *L;
    Reader r_;
    int values_ = 0;
    bool indexed_ = false;
    int index_ = LUA_NOREF;
    std::vector<Entry> entries_;
    std::vector<Value> key_values_;
    std::vector<std::vector<Value>> contents_;
    std::vector<std::string> strings_;
    std::vector<std::pair<std::uint64_t, std::array<int, 4>>> protos_;
    std::unordered_set<const void *> used_;
    ImportStats stats_;
};

} // namespace

std::string Export(Dumps *dumps) {
    Exporter e(detail::Lua());
    e.dumps_ = dumps;
    return e.Run();
}

std::vector<std::uint64_t> MissingCode(const std::string &data) {
    Importer i(detail::Lua(), data);
    i.check_only_ = true;
    std::string error;
    i.Run(error, nullptr);
    return i.missing_;
}

bool Import(const std::string &data, const Dumps *dumps, std::string &error, ImportStats *stats) {
    Importer i(detail::Lua(), data);
    i.dumps_ = dumps;
    return i.Run(error, stats);
}

} // namespace LuaSnapshot
