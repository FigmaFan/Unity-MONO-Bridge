#pragma once
// ===============================
// mono_resolver.hpp (v1.0)
// - Lazy export binding
// - RAII ThreadScope
// - Status-based Result<T>, analog zu il2cpp_resolver
// - Enum-Reflection over Mono-API
// ===============================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <type_traits>
#include <memory>
#include <vector>
#include <algorithm>
#include <cstring>

#include "../../../Loggy/loggy.hpp"

#define MONO_DLL_CANDIDATE_1 "mono-2.0-bdwgc.dll"
#define MONO_DLL_CANDIDATE_2 "mono-2.0-sgen.dll"
#define MONO_DLL_CANDIDATE_3 "mono.dll"

enum class MonoStatus : uint32_t {
    OK = 0,
    MonoModuleNotFound,
    GetProcAddressFailed,

    Missing_get_root_domain,
    Missing_thread_attach,
    Missing_domain_assembly_open,
    Missing_assembly_get_image,
    Missing_class_from_name,
    Missing_class_get_method_from_name,
    Missing_class_get_field_from_name,
    Missing_field_get_set,
    Missing_runtime_invoke,
    Missing_object_unbox,
    Missing_string_new,

    DomainUnavailable,
    AssemblyNotFound,
    ImageUnavailable,
    ClassNotFound,
    MethodNotFound,
    FieldNotFound,
    InvalidArgs,
    ThreadAttachUnavailable,
    InvokeException,
};

inline const char* to_string(MonoStatus s) {
    switch (s) {
    case MonoStatus::OK:                                 return "OK";
    case MonoStatus::MonoModuleNotFound:                 return "MonoModuleNotFound";
    case MonoStatus::GetProcAddressFailed:               return "GetProcAddressFailed";
    case MonoStatus::Missing_get_root_domain:            return "Missing_get_root_domain";
    case MonoStatus::Missing_thread_attach:              return "Missing_thread_attach";
    case MonoStatus::Missing_domain_assembly_open:       return "Missing_domain_assembly_open";
    case MonoStatus::Missing_assembly_get_image:         return "Missing_assembly_get_image";
    case MonoStatus::Missing_class_from_name:            return "Missing_class_from_name";
    case MonoStatus::Missing_class_get_method_from_name: return "Missing_class_get_method_from_name";
    case MonoStatus::Missing_class_get_field_from_name:  return "Missing_class_get_field_from_name";
    case MonoStatus::Missing_field_get_set:              return "Missing_field_get_set";
    case MonoStatus::Missing_runtime_invoke:             return "Missing_runtime_invoke";
    case MonoStatus::Missing_object_unbox:               return "Missing_object_unbox";
    case MonoStatus::Missing_string_new:                 return "Missing_string_new";
    case MonoStatus::DomainUnavailable:                  return "DomainUnavailable";
    case MonoStatus::AssemblyNotFound:                   return "AssemblyNotFound";
    case MonoStatus::ImageUnavailable:                   return "ImageUnavailable";
    case MonoStatus::ClassNotFound:                      return "ClassNotFound";
    case MonoStatus::MethodNotFound:                     return "MethodNotFound";
    case MonoStatus::FieldNotFound:                      return "FieldNotFound";
    case MonoStatus::InvalidArgs:                        return "InvalidArgs";
    case MonoStatus::ThreadAttachUnavailable:            return "ThreadAttachUnavailable";
    case MonoStatus::InvokeException:                    return "InvokeException";
    default:                                             return "Unknown";
    }
}

namespace mono {

    template <typename T>
    struct Result { MonoStatus status{ MonoStatus::OK }; T value{}; explicit operator bool() const { return status == MonoStatus::OK; } };
    template <>
    struct Result<void> { MonoStatus status{ MonoStatus::OK }; explicit operator bool() const { return status == MonoStatus::OK; } };

    namespace _internal {

        struct _MonoDomain;
        struct _MonoAssembly;
        struct _MonoAssemblyName;
        struct _MonoImage;
        struct _MonoProperty;
        struct _MonoClass;
        struct _MonoMethod;
        struct _MonoString;
        struct _MonoArray;
        struct _MonoObject;
        struct _MonoClassField;
        struct _MonoVTable;
        struct _MonoType;
        struct _MonoThread;

        using MonoDomain = _MonoDomain;
        using MonoAssembly = _MonoAssembly;
        using MonoAssemblyName = _MonoAssemblyName;
        using MonoImage = _MonoImage;
        using MonoProperty = _MonoProperty;
        using MonoClass = _MonoClass;
        using MonoMethod = _MonoMethod;
        using MonoString = _MonoString;
        using MonoArray = _MonoArray;
        using MonoObject = _MonoObject;
        using MonoClassField = _MonoClassField;
        using MonoVTable = _MonoVTable;
        using MonoType = _MonoType;
        using MonoThread = _MonoThread;

        inline HMODULE g_mono_module = nullptr;
        inline std::unordered_map<std::string, MonoAssembly*> g_assembly_cache;
        inline std::mutex g_cache_mtx;

        // ===== Module-Handling =====
        inline Result<HMODULE> ensure_mono_module() {
            if (g_mono_module) return { MonoStatus::OK, g_mono_module };

            const char* candidates[] = {
                MONO_DLL_CANDIDATE_1,
                MONO_DLL_CANDIDATE_2,
                MONO_DLL_CANDIDATE_3
            };

            constexpr int MAX_ITERATIONS = 200;
            constexpr int SLEEP_MS = 10;

            for (int i = 0; i < MAX_ITERATIONS && !g_mono_module; ++i) {
                for (auto* name : candidates) {
                    HMODULE h = ::GetModuleHandleA(name);
                    if (h) {
                        g_mono_module = h;
                        break;
                    }
                }
                if (!g_mono_module) ::Sleep(SLEEP_MS);
            }

            if (!g_mono_module) return { MonoStatus::MonoModuleNotFound, nullptr };
            return { MonoStatus::OK, g_mono_module };
        }

        template <class T>
        inline Result<T> resolve_export(std::string_view name) {
            auto mod = ensure_mono_module();
            if (!mod) return { mod.status, nullptr };
            auto* p = reinterpret_cast<T>(::GetProcAddress(mod.value, std::string(name).c_str()));
            if (!p) return { MonoStatus::GetProcAddressFailed, nullptr };
            return { MonoStatus::OK, p };
        }

        // ===== Gebundene Funktionspointer =====

        // Domain / Thread
        inline MonoDomain* (__cdecl* mono_get_root_domain)() = nullptr;
        inline MonoThread* (__cdecl* mono_thread_attach)(MonoDomain*) = nullptr;
        inline void(__cdecl* mono_thread_detach)(MonoThread*) = nullptr; // optional
        inline MonoThread* (__cdecl* mono_thread_current)() = nullptr;           // optional

        // Assembly & Image
        inline MonoAssembly* (__cdecl* mono_domain_assembly_open)(MonoDomain*, const char*) = nullptr;
        inline MonoImage* (__cdecl* mono_assembly_get_image)(MonoAssembly*) = nullptr;
        inline void(__cdecl* mono_assembly_foreach)(void (*)(MonoAssembly*, void*), void*) = nullptr;
        inline MonoAssemblyName* (__cdecl* mono_assembly_get_name)(MonoAssembly*) = nullptr;
        inline const char* (__cdecl* mono_assembly_name_get_name)(MonoAssemblyName*) = nullptr;

        // Class & Methods & Fields
        inline MonoClass* (__cdecl* mono_class_from_name)(MonoImage*, const char*, const char*) = nullptr;
        inline MonoClass* (__cdecl* mono_class_get_parent)(MonoClass*) = nullptr;
        inline MonoMethod* (__cdecl* mono_class_get_method_from_name)(MonoClass*, const char*, int) = nullptr;
        inline MonoProperty* (__cdecl* mono_class_get_property_from_name)(MonoClass*, const char*) = nullptr;
        inline MonoMethod* (__cdecl* mono_property_get_get_method)(MonoProperty*) = nullptr;
        inline MonoClassField* (__cdecl* mono_class_get_field_from_name)(MonoClass*, const char*) = nullptr;
        inline MonoClassField* (__cdecl* mono_class_get_fields)(MonoClass*, void**) = nullptr;
        inline const char* (__cdecl* mono_field_get_name)(MonoClassField*) = nullptr;

        inline void(__cdecl* mono_field_get_value)(MonoObject*, MonoClassField*, void*) = nullptr;
        inline void(__cdecl* mono_field_set_value)(MonoObject*, MonoClassField*, void*) = nullptr;

        inline MonoVTable* (__cdecl* mono_class_vtable)(MonoDomain*, MonoClass*) = nullptr;
        inline void(__cdecl* mono_field_static_get_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
        inline void(__cdecl* mono_field_static_set_value)(MonoVTable*, MonoClassField*, void*) = nullptr;

        inline MonoObject* (__cdecl* mono_field_get_value_object)(MonoDomain*, MonoClassField*, MonoObject*) = nullptr;

        // Object / String / Invoke
        inline MonoObject* (__cdecl* mono_object_new)(MonoDomain*, MonoClass*) = nullptr;
        inline MonoObject* (__cdecl* mono_runtime_invoke)(MonoMethod*, void*, void**, MonoObject**) = nullptr;
        inline void* (__cdecl* mono_object_unbox)(MonoObject*) = nullptr;

        inline MonoString* (__cdecl* mono_string_new)(MonoDomain*, const char*) = nullptr;
        inline char* (__cdecl* mono_string_to_utf8)(MonoString*) = nullptr;
        inline void(__cdecl* mono_free)(void*) = nullptr;

        inline MonoClass* (__cdecl* mono_get_enum_class)() = nullptr; // optional, meist nicht nötig
        inline mono::_internal::MonoType* (__cdecl* mono_field_get_type)(MonoClassField*) = nullptr; // optional
        inline mono::_internal::MonoClass* (__cdecl* mono_type_get_class)(MonoType*) = nullptr;      // optional
        inline mono::_internal::MonoDomain* (__cdecl* mono_object_get_domain)(MonoObject*) = nullptr; // optional

        // Array
        inline uintptr_t(__cdecl* mono_array_length)(MonoArray*) = nullptr;
        inline void* (__cdecl* mono_array_addr_with_size)(MonoArray*, int, uintptr_t) = nullptr;

        // Optional
        inline mono::_internal::MonoClass* (__cdecl* mono_object_get_class)(MonoObject*) = nullptr;
        inline bool(__cdecl* mono_class_is_enum)(MonoClass*) = nullptr;

        // ===== Lazy Binding =====
        inline MonoStatus ensure_exports() {
            auto bind_req = [](auto& dst, const char* name, MonoStatus err) -> MonoStatus {
                if (!dst) {
                    auto r = resolve_export<std::remove_reference_t<decltype(dst)>>(name);
                    if (r.status != MonoStatus::OK || !r.value) return err;
                    dst = r.value;
                }
                return MonoStatus::OK;
                };
            auto bind_opt = [](auto& dst, const char* name) {
                if (!dst) {
                    auto r = resolve_export<std::remove_reference_t<decltype(dst)>>(name);
                    if (r.status == MonoStatus::OK && r.value) dst = r.value;
                }
                };

            // Domain / Thread
            if (auto s = bind_req(mono_get_root_domain, "mono_get_root_domain", MonoStatus::Missing_get_root_domain); s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_thread_attach, "mono_thread_attach", MonoStatus::Missing_thread_attach);   s != MonoStatus::OK) return s;
            bind_opt(mono_thread_detach, "mono_thread_detach");
            bind_opt(mono_thread_current, "mono_thread_current");

            // Assembly / Image
            if (auto s = bind_req(mono_domain_assembly_open, "mono_domain_assembly_open", MonoStatus::Missing_domain_assembly_open); s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_assembly_get_image, "mono_assembly_get_image", MonoStatus::Missing_assembly_get_image);   s != MonoStatus::OK) return s;
            bind_opt(mono_assembly_foreach, "mono_assembly_foreach");
            bind_opt(mono_assembly_get_name, "mono_assembly_get_name");
            bind_opt(mono_assembly_name_get_name, "mono_assembly_name_get_name");

            // Class / Method / Field
            if (auto s = bind_req(mono_class_from_name, "mono_class_from_name", MonoStatus::Missing_class_from_name);            s != MonoStatus::OK) return s;
            bind_opt(mono_class_get_parent, "mono_class_get_parent");
            if (auto s = bind_req(mono_class_get_method_from_name, "mono_class_get_method_from_name", MonoStatus::Missing_class_get_method_from_name); s != MonoStatus::OK) return s;
            bind_opt(mono_class_get_property_from_name, "mono_class_get_property_from_name");
            bind_opt(mono_property_get_get_method, "mono_property_get_get_method");
            if (auto s = bind_req(mono_class_get_field_from_name, "mono_class_get_field_from_name", MonoStatus::Missing_class_get_field_from_name);  s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_class_get_fields, "mono_class_get_fields", MonoStatus::Missing_class_get_field_from_name);    s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_field_get_name, "mono_field_get_name", MonoStatus::Missing_class_get_field_from_name);    s != MonoStatus::OK) return s;

            if (auto s = bind_req(mono_field_get_value, "mono_field_get_value", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_field_set_value, "mono_field_set_value", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;

            if (auto s = bind_req(mono_class_vtable, "mono_class_vtable", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_field_static_get_value, "mono_field_static_get_value", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_field_static_set_value, "mono_field_static_set_value", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;

            if (auto s = bind_req(mono_field_get_value_object, "mono_field_get_value_object", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;

            // Object / Invoke / Unbox
            if (auto s = bind_req(mono_object_new, "mono_object_new", MonoStatus::GetProcAddressFailed);     s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_runtime_invoke, "mono_runtime_invoke", MonoStatus::Missing_runtime_invoke);   s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_object_unbox, "mono_object_unbox", MonoStatus::Missing_object_unbox);     s != MonoStatus::OK) return s;

            // Strings
            bind_opt(mono_string_new, "mono_string_new");
            bind_opt(mono_string_to_utf8, "mono_string_to_utf8");
            bind_opt(mono_free, "mono_free");

            // Array
            bind_opt(mono_array_length, "mono_array_length");
            bind_opt(mono_array_addr_with_size, "mono_array_addr_with_size");

            // Optional helpers
            bind_opt(mono_get_enum_class, "mono_get_enum_class");
            bind_opt(mono_field_get_type, "mono_field_get_type");
            bind_opt(mono_type_get_class, "mono_type_get_class");
            bind_opt(mono_object_get_domain, "mono_object_get_domain");
            bind_opt(mono_object_get_class, "mono_object_get_class");
            bind_opt(mono_class_is_enum, "mono_class_is_enum");

            return MonoStatus::OK;
        }

        // Assembly-Lookup + Cache
        inline Result<MonoAssembly*> find_assembly(std::string_view assembly_name) {
            if (assembly_name.empty()) return { MonoStatus::InvalidArgs, nullptr };
            LOG(LogLevel::INFO, "ensure_exports...");
            if (auto s = ensure_exports(); s != MonoStatus::OK) return { s, nullptr };
            LOG(LogLevel::INFO, "ensure_exports done");

            {
                std::scoped_lock lk(g_cache_mtx);
                if (auto it = g_assembly_cache.find(std::string(assembly_name)); it != g_assembly_cache.end())
                    return { MonoStatus::OK, it->second };
            }

            auto* domain = mono_get_root_domain ? mono_get_root_domain() : nullptr;
            if (!domain) {
                LOG(LogLevel::WARN, "get_root_domain failed");
                return { MonoStatus::DomainUnavailable, nullptr };
            }

            MonoAssembly* ass = nullptr;

            // Simple name: "Assembly-CSharp.dll" -> "Assembly-CSharp", "Assembly-CSharp" -> "Assembly-CSharp"
            std::string aname_str(assembly_name);
            std::string simple_name = aname_str;
            if (simple_name.size() >= 4 && simple_name.compare(simple_name.size() - 4, 4, ".dll") == 0)
                simple_name.resize(simple_name.size() - 4);

            if (mono_assembly_foreach && mono_assembly_get_name && mono_assembly_name_get_name) {
                struct Ctx { MonoAssembly* found = nullptr; const char* simple = nullptr; } ctx;
                ctx.simple = simple_name.c_str();
                mono_assembly_foreach([](MonoAssembly* a, void* ud) {
                    Ctx* c = static_cast<Ctx*>(ud);
                    MonoAssemblyName* aname = mono_assembly_get_name(a);
                    if (!aname) return;
                    const char* n = mono_assembly_name_get_name(aname);
                    if (n && c->simple && std::strcmp(n, c->simple) == 0)
                        c->found = a;
                }, &ctx);
                ass = ctx.found;
                if (ass) LOG(LogLevel::INFO, "found loaded assembly: ", simple_name);
            }

            if (!ass) {
                LOG(LogLevel::INFO, "assembly_open ", aname_str, " ...");
                ass = mono_domain_assembly_open(domain, aname_str.c_str());
                if (!ass) {
                    LOG(LogLevel::WARN, "assembly_open failed");
                    return { MonoStatus::AssemblyNotFound, nullptr };
                }
                LOG(LogLevel::INFO, "assembly_open done");
            }

            {
                std::scoped_lock lk(g_cache_mtx);
                g_assembly_cache.emplace(aname_str, ass);
            }
            return { MonoStatus::OK, ass };
        }

    } // _internal

    // ---------- ThreadScope (RAII) ----------
    struct ThreadScope {
        bool       attached_by_us{ false };
        _internal::MonoThread* attached_thread{ nullptr };
        inline static thread_local int depth{ 0 };

        ThreadScope() {
            if (_internal::ensure_exports() != MonoStatus::OK) return;

            if (!_internal::mono_get_root_domain || !_internal::mono_thread_attach) return;

            auto* dom = _internal::mono_get_root_domain();
            if (!dom) return;

            attached_thread = _internal::mono_thread_attach(dom);
            attached_by_us = (attached_thread != nullptr);
            if (attached_by_us) ++depth;
        }

        ~ThreadScope() noexcept {
            if (depth > 0) --depth;
            if (attached_by_us && depth == 0 && _internal::mono_thread_detach && attached_thread) {
                _internal::mono_thread_detach(attached_thread);
            }
        }
    };

    // ---------- Init / Cleanup ----------
    inline MonoStatus init() {
        auto mod = _internal::ensure_mono_module();
        if (!mod) return mod.status;
        return _internal::ensure_exports();
    }

    inline void cleanup() {
        std::scoped_lock lk(_internal::g_cache_mtx);
        _internal::g_assembly_cache.clear();
    }

    // ---------- Class & Method ----------

    inline Result<_internal::MonoClass*>
        find_class(const std::string& ns, const std::string& class_name, const std::string& assembly_name) {
        ThreadScope scope;
        if (class_name.empty() || assembly_name.empty())
            return { MonoStatus::InvalidArgs, nullptr };

        LOG(LogLevel::INFO, "find_assembly...");
        auto a = _internal::find_assembly(assembly_name);
        if (!a) {
            LOG(LogLevel::WARN, "find_assembly failed: ", to_string(a.status));
            return { a.status, nullptr };
        }
        LOG(LogLevel::INFO, "find_assembly done");

        LOG(LogLevel::INFO, "get_image...");
        auto* img = _internal::mono_assembly_get_image ? _internal::mono_assembly_get_image(a.value) : nullptr;
        if (!img) {
            LOG(LogLevel::WARN, "get_image failed");
            return { MonoStatus::ImageUnavailable, nullptr };
        }
        LOG(LogLevel::INFO, "get_image done");

        LOG(LogLevel::INFO, "class_from_name...");
        auto* klass = _internal::mono_class_from_name(img, ns.c_str(), class_name.c_str());
        if (!klass) {
            LOG(LogLevel::WARN, "class_from_name failed");
            return { MonoStatus::ClassNotFound, nullptr };
        }
        LOG(LogLevel::INFO, "class_from_name done");
        return { MonoStatus::OK, klass };
    }

    inline Result<_internal::MonoMethod*>
        get_method(const std::string& ns, const std::string& class_name, const std::string& method_name,
            const std::string& assembly_name, std::optional<int> param_count = std::nullopt) {
        LOG(LogLevel::INFO, "ThreadScope creating...");
        ThreadScope scope;
        LOG(LogLevel::INFO, "ThreadScope done");
        if (class_name.empty() || method_name.empty() || assembly_name.empty())
            return { MonoStatus::InvalidArgs, nullptr };

        LOG(LogLevel::INFO, "find_class...");
        auto c = find_class(ns, class_name, assembly_name);
        if (!c) {
            LOG(LogLevel::WARN, "find_class failed: ", to_string(c.status));
            return { c.status, nullptr };
        }
        LOG(LogLevel::INFO, "find_class done");

        LOG(LogLevel::INFO, "get_method_from_name...");
        using MM = _internal::MonoMethod*;
        MM mi = nullptr;

        if (param_count.has_value()) {
            mi = _internal::mono_class_get_method_from_name(c.value, method_name.c_str(), *param_count);
        }
        else {
            constexpr int MAX_PARAM_COUNT = 16;
            for (int i = 0; i <= MAX_PARAM_COUNT && !mi; ++i)
                mi = _internal::mono_class_get_method_from_name(c.value, method_name.c_str(), i);
        }

        if (!mi) {
            LOG(LogLevel::WARN, "get_method_from_name failed");
            return { MonoStatus::MethodNotFound, nullptr };
        }
        LOG(LogLevel::INFO, "get_method_from_name done");
        return { MonoStatus::OK, mi };
    }

    // ---------- Fields ----------

    inline Result<_internal::MonoClassField*>
        get_field(const std::string& ns, const std::string& class_name,
            const std::string& field_name, const std::string& assembly_name) {
        ThreadScope scope;
        if (class_name.empty() || field_name.empty() || assembly_name.empty())
            return { MonoStatus::InvalidArgs, nullptr };

        auto c = find_class(ns, class_name, assembly_name);
        if (!c) return { c.status, nullptr };

        auto* fld = _internal::mono_class_get_field_from_name(c.value, field_name.c_str());
        if (!fld) return { MonoStatus::FieldNotFound, nullptr };

        return { MonoStatus::OK, fld };
    }

    template <class T>
    inline Result<T> get_object_field_value(void* instance,
        const std::string& ns, const std::string& class_name,
        const std::string& field_name, const std::string& assembly_name) {
        ThreadScope scope;
        if (!instance) return { MonoStatus::InvalidArgs, T{} };

        auto f = get_field(ns, class_name, field_name, assembly_name);
        if (!f) return { f.status, T{} };

        T out{};
        _internal::mono_field_get_value(reinterpret_cast<_internal::MonoObject*>(instance), f.value, &out);
        return { MonoStatus::OK, out };
    }

    template <class T>
    inline MonoStatus set_object_field_value(void* instance,
        const std::string& ns, const std::string& class_name,
        const std::string& field_name, const T& value,
        const std::string& assembly_name) {
        ThreadScope scope;
        if (!instance) return MonoStatus::InvalidArgs;

        auto f = get_field(ns, class_name, field_name, assembly_name);
        if (!f) return f.status;

        _internal::mono_field_set_value(reinterpret_cast<_internal::MonoObject*>(instance), f.value, const_cast<T*>(&value));
        return MonoStatus::OK;
    }

    template <class T>
    inline Result<T> get_static_field_value(_internal::MonoClass* klass, const std::string& field_name) {
        ThreadScope scope;
        if (!klass || field_name.empty()) return { MonoStatus::InvalidArgs, T{} };

        auto* fld = _internal::mono_class_get_field_from_name(klass, field_name.c_str());
        if (!fld) return { MonoStatus::FieldNotFound, T{} };

        auto* domain = _internal::mono_get_root_domain ? _internal::mono_get_root_domain() : nullptr;
        if (!domain) return { MonoStatus::DomainUnavailable, T{} };

        auto* vt = _internal::mono_class_vtable(domain, klass);
        if (!vt) return { MonoStatus::FieldNotFound, T{} };

        T out{};
        _internal::mono_field_static_get_value(vt, fld, &out);
        return { MonoStatus::OK, out };
    }

    template <class T>
    inline MonoStatus set_static_field_value(_internal::MonoClass* klass,
        const std::string& field_name, const T& value) {
        ThreadScope scope;
        if (!klass || field_name.empty()) return MonoStatus::InvalidArgs;

        auto* fld = _internal::mono_class_get_field_from_name(klass, field_name.c_str());
        if (!fld) return MonoStatus::FieldNotFound;

        auto* domain = _internal::mono_get_root_domain ? _internal::mono_get_root_domain() : nullptr;
        if (!domain) return MonoStatus::DomainUnavailable;

        auto* vt = _internal::mono_class_vtable(domain, klass);
        if (!vt) return MonoStatus::FieldNotFound;

        _internal::mono_field_static_set_value(vt, fld, const_cast<T*>(&value));
        return MonoStatus::OK;
    }

    // ---------- Object Creation ----------

    template <typename T = void*>
    inline Result<T> create_object(_internal::MonoClass* klass) {
        ThreadScope scope;
        if (!klass) return { MonoStatus::ClassNotFound, nullptr };

        if (!_internal::mono_object_new) {
            if (auto s = _internal::ensure_exports(); s != MonoStatus::OK)
                return { s, nullptr };
        }
        if (!_internal::mono_object_new) return { MonoStatus::GetProcAddressFailed, nullptr };

        auto* domain = _internal::mono_get_root_domain ? _internal::mono_get_root_domain() : nullptr;
        if (!domain) return { MonoStatus::DomainUnavailable, nullptr };

        auto* obj = _internal::mono_object_new(domain, klass);
        if (!obj) return { MonoStatus::InvalidArgs, nullptr };
        return { MonoStatus::OK, reinterpret_cast<T>(obj) };
    }

    template <typename T = void*, typename... CtorArgs>
    inline Result<T> create_object(const std::string& ns, const std::string& class_name,
        const std::string& assembly_name, CtorArgs... ctor_args) {
        ThreadScope scope;
        auto klass = find_class(ns, class_name, assembly_name);
        if (!klass) return { klass.status, nullptr };

        auto r = create_object<T>(klass.value);
        if (!r) return r;

        if constexpr (sizeof...(CtorArgs) > 0) {
            auto mi_ctor = get_method(ns, class_name, ".ctor", assembly_name, sizeof...(CtorArgs));
            if (!mi_ctor) return { mi_ctor.status, nullptr };

            // ctor_args → runtime_invoke, Ergebnis ignorieren
            void* argv[sizeof...(CtorArgs)] = { reinterpret_cast<void*>(&ctor_args)... };
            _internal::MonoObject* exc = nullptr;
            _internal::MonoObject* res = _internal::mono_runtime_invoke(mi_ctor.value,
                reinterpret_cast<void*>(r.value),
                argv, &exc);
            (void)res;
            if (exc) return { MonoStatus::InvokeException, nullptr };
        }
        return r;
    }

    // ---------- Strings ----------

    namespace String {
        inline Result<void*> CreateNewString(const std::string& s) {
            ThreadScope scope;
            if (s.empty()) return { MonoStatus::InvalidArgs, nullptr };

            if (!_internal::mono_string_new) {
                (void)_internal::ensure_exports();
                if (!_internal::mono_string_new) return { MonoStatus::Missing_string_new, nullptr };
            }

            auto* domain = _internal::mono_get_root_domain ? _internal::mono_get_root_domain() : nullptr;
            if (!domain) return { MonoStatus::DomainUnavailable, nullptr };

            auto* str = _internal::mono_string_new(domain, s.c_str());
            if (!str) return { MonoStatus::InvalidArgs, nullptr };

            return { MonoStatus::OK, str };
        }

        inline std::string convert_to_std_string(void* p_sys_str) {
            ThreadScope scope;
            if (!p_sys_str) return {};

            auto* mono_str = reinterpret_cast<_internal::MonoString*>(p_sys_str);
            if (!_internal::mono_string_to_utf8) {
                (void)_internal::ensure_exports();
                if (!_internal::mono_string_to_utf8) return {};
            }

            char* utf8 = _internal::mono_string_to_utf8(mono_str);
            if (!utf8) return {};

            std::string out(utf8);
            if (_internal::mono_free) _internal::mono_free(utf8);
            return out;
        }
    } // namespace String

    inline Result<void*> CreateNewString(const std::string& s) { return String::CreateNewString(s); }
    inline std::string   convert_to_std_string(void* p_sys_str) { return String::convert_to_std_string(p_sys_str); }

    // ---------- Managed Calls (mono_runtime_invoke) ----------

    template <typename Ret, typename... Args>
    inline auto call_function(_internal::MonoMethod* method, void* instance, Args... args)
        -> std::conditional_t<std::is_void_v<Ret>, Result<void>, Result<Ret>>
    {
        ThreadScope scope;
        using R = std::conditional_t<std::is_void_v<Ret>, Result<void>, Result<Ret>>;

        if (!method) {
            if constexpr (std::is_void_v<Ret>) return R{ MonoStatus::MethodNotFound };
            else                               return R{ MonoStatus::MethodNotFound, Ret{} };
        }

        if (!_internal::mono_runtime_invoke || !_internal::mono_object_unbox) {
            if (auto s = _internal::ensure_exports(); s != MonoStatus::OK) {
                if constexpr (std::is_void_v<Ret>) return R{ s };
                else                               return R{ s, Ret{} };
            }
        }

        _internal::MonoObject* exc = nullptr;
        _internal::MonoObject* result = nullptr;

        if constexpr (sizeof...(Args) == 0) {
            result = _internal::mono_runtime_invoke(method, instance, nullptr, &exc);
        } else {
            void* argv[sizeof...(Args)] = { reinterpret_cast<void*>(&args)... };
            result = _internal::mono_runtime_invoke(method, instance, argv, &exc);
        }

        if (exc) {
            if constexpr (std::is_void_v<Ret>) return R{ MonoStatus::InvokeException };
            else                               return R{ MonoStatus::InvokeException, Ret{} };
        }

        if constexpr (std::is_void_v<Ret>) {
            return R{ MonoStatus::OK };
        }
        else if constexpr (std::is_pointer_v<Ret>) {
            // Referenztypen: direkter MonoObject*-Return
            return R{ MonoStatus::OK, reinterpret_cast<Ret>(result) };
        }
        else {
            if (!result) return R{ MonoStatus::OK, Ret{} };
            void* boxed = _internal::mono_object_unbox(result);
            if (!boxed) return R{ MonoStatus::OK, Ret{} };

            Ret out{};
            std::memcpy(&out, boxed, sizeof(Ret));
            return R{ MonoStatus::OK, out };
        }
    }

    // ---------- Arrays ----------

    inline Result<int> array_get_length_1d(void* arr) {
        ThreadScope scope;
        if (!arr) return { MonoStatus::InvalidArgs, 0 };

        if (!_internal::mono_array_length) {
            (void)_internal::ensure_exports();
            if (!_internal::mono_array_length) return { MonoStatus::GetProcAddressFailed, 0 };
        }

        auto len = static_cast<int>(_internal::mono_array_length(reinterpret_cast<_internal::MonoArray*>(arr)));
        return { MonoStatus::OK, len };
    }

    template <typename Ret>
    inline Result<Ret> array_get_element_1d(void* arr, uintptr_t idx) {
        ThreadScope scope;
        if (!arr) return { MonoStatus::InvalidArgs, Ret{} };

        if (!_internal::mono_array_length || !_internal::mono_array_addr_with_size) {
            (void)_internal::ensure_exports();
            if (!_internal::mono_array_length || !_internal::mono_array_addr_with_size)
                return { MonoStatus::GetProcAddressFailed, Ret{} };
        }

        auto* mono_arr = reinterpret_cast<_internal::MonoArray*>(arr);
        uintptr_t len = _internal::mono_array_length(mono_arr);
        if (idx >= len) return { MonoStatus::InvalidArgs, Ret{} };

        void* elemPtr = _internal::mono_array_addr_with_size(mono_arr, static_cast<int>(sizeof(Ret)), idx);
        Ret out{};
        std::memcpy(&out, elemPtr, sizeof(Ret));
        return { MonoStatus::OK, out };
    }

    // ---------- Enum Utils ----------

    namespace Enum {
        struct Key {
            std::string ns;
            std::string name;
            std::string assembly;
            bool operator==(const Key& o) const noexcept {
                return ns == o.ns && name == o.name && assembly == o.assembly;
            }
        };

        struct KeyHash {
            size_t operator()(const Key& k) const noexcept {
                std::hash<std::string> H;
                size_t h = H(k.ns);
                h ^= (H(k.name) << 1);
                h ^= (H(k.assembly) << 2);
                return h;
            }
        };

        struct EnumEntry {
            int32_t     value;
            std::string name;
        };

        inline Result<std::shared_ptr<const std::vector<EnumEntry>>>
            get_entries(std::string_view ns, std::string_view name, std::string_view assembly) {
            ThreadScope scope;

            static std::unordered_map<Key, std::shared_ptr<const std::vector<EnumEntry>>, KeyHash> g_cache;
            static std::mutex m;

            Key key{ std::string(ns), std::string(name), std::string(assembly) };

            {
                std::lock_guard<std::mutex> lk(m);
                if (auto it = g_cache.find(key); it != g_cache.end())
                    return { MonoStatus::OK, it->second };
            }

            auto klass = find_class(key.ns, key.name, key.assembly);
            if (!klass) return { klass.status, nullptr };

            // Wenn mono_class_is_enum fehlt, überspringen wir den Check
            if (_internal::mono_class_is_enum && !_internal::mono_class_is_enum(klass.value))
                return { MonoStatus::InvalidArgs, nullptr };

            auto entries = std::make_shared<std::vector<EnumEntry>>();
            entries->reserve(16);

            void* iter = nullptr;
            while (true) {
                auto* fi = _internal::mono_class_get_fields(klass.value, &iter);
                if (!fi) break;

                const char* fname = _internal::mono_field_get_name(fi);
                if (!fname || std::strcmp(fname, "value__") == 0) continue;

                // Enum-Werte via value_object + unbox holen
                auto* domain = _internal::mono_get_root_domain ? _internal::mono_get_root_domain() : nullptr;
                if (!domain) break;

                _internal::MonoObject* valObj = _internal::mono_field_get_value_object(domain, fi, nullptr);
                if (!valObj) continue;

                void* boxed = _internal::mono_object_unbox(valObj);
                if (!boxed) continue;

                int32_t v = 0;
                std::memcpy(&v, boxed, sizeof(int32_t));

                if (std::strlen(fname))
                    entries->push_back({ v, std::string(fname) });
            }

            std::sort(entries->begin(), entries->end(),
                [](auto& a, auto& b) {
                    return a.value < b.value || (a.value == b.value && a.name < b.name);
                });

            entries->erase(
                std::unique(entries->begin(), entries->end(),
                    [](auto& a, auto& b) { return a.value == b.value && a.name == b.name; }),
                entries->end()
            );

            {
                std::lock_guard<std::mutex> lk(m);
                g_cache.emplace(std::move(key), entries);
            }

            return { MonoStatus::OK, entries };
        }

        inline Result<std::string>
            name_of(std::string_view ns, std::string_view name, std::string_view assembly, int32_t value) {
            auto entries = get_entries(ns, name, assembly);
            if (!entries) return { entries.status, {} };

            const auto& vec = *entries.value;
            auto it = std::find_if(vec.begin(), vec.end(),
                [&](auto& e) { return e.value == value; });
            if (it == vec.end()) return { MonoStatus::FieldNotFound, {} };
            return { MonoStatus::OK, it->name };
        }

        inline Result<int32_t>
            value_of(std::string_view ns, std::string_view name, std::string_view assembly, std::string_view literal) {
            auto entries = get_entries(ns, name, assembly);
            if (!entries) return { entries.status, 0 };

            const auto& vec = *entries.value;
            auto it = std::find_if(vec.begin(), vec.end(),
                [&](auto& e) { return e.name == literal; });
            if (it == vec.end()) return { MonoStatus::FieldNotFound, 0 };
            return { MonoStatus::OK, it->value };
        }

        struct IndexMap {
            std::shared_ptr<const std::vector<EnumEntry>> entries;
            std::unordered_map<int32_t, int> value_to_index;

            int index_of(int32_t value) const {
                if (auto it = value_to_index.find(value); it != value_to_index.end())
                    return it->second;
                return 0;
            }
        };

        inline Result<IndexMap>
            index_map(std::string_view ns, std::string_view name, std::string_view assembly) {
            IndexMap M{};
            auto entries = get_entries(ns, name, assembly);
            if (!entries) return { entries.status, {} };

            M.entries = entries.value;
            M.value_to_index.reserve(M.entries->size());
            for (int i = 0; i < static_cast<int>(M.entries->size()); ++i)
                M.value_to_index.emplace((*M.entries)[i].value, i);

            return { MonoStatus::OK, std::move(M) };
        }

    } // namespace Enum

    // ---------- Unity Helpers ----------

    // FindAnyObjectByType: Wrapper für UnityEngine.Object.FindAnyObjectByType(Type)
    // In Unity/Mono kann MonoClass* oft direkt als System.Type verwendet werden
    inline Result<void*> FindAnyObjectByType(_internal::MonoClass* klass) {
        ThreadScope scope;
        if (!klass) return { MonoStatus::InvalidArgs, nullptr };

        // UnityEngine.Object Klasse finden
        auto obj_class = find_class("UnityEngine", "Object", "UnityEngine.CoreModule");
        if (!obj_class) return { obj_class.status, nullptr };

        // FindAnyObjectByType Methode finden (statische Methode mit 1 Parameter: Type)
        auto find_method = get_method("UnityEngine", "Object", "FindAnyObjectByType", "UnityEngine.CoreModule", 1);
        if (!find_method) return { find_method.status, nullptr };

        // In Unity/Mono kann MonoClass* direkt als System.Type verwendet werden
        // Wir casten klass direkt zu void* (Type ist ein managed Object)
        void* type_obj = reinterpret_cast<void*>(klass);

        // Methode aufrufen (statisch, instance = nullptr)
        void* argv[1] = { type_obj };
        _internal::MonoObject* exc = nullptr;
        _internal::MonoObject* result = _internal::mono_runtime_invoke(
            find_method.value,
            nullptr, // statische Methode
            argv,
            &exc
        );

        if (exc) return { MonoStatus::InvokeException, nullptr };
        if (!result) return { MonoStatus::OK, nullptr }; // null Object (kein Objekt gefunden)

        return { MonoStatus::OK, result };
    }

    // Overload: FindAnyObjectByType mit Namespace, Class-Name und Assembly
    inline Result<void*> FindAnyObjectByType(
        const std::string& ns, 
        const std::string& class_name, 
        const std::string& assembly_name) {
        auto klass = find_class(ns, class_name, assembly_name);
        if (!klass) return { klass.status, nullptr };
        return FindAnyObjectByType(klass.value);
    }

    // MonoSingleton<T>-style Instance-Zugriff: Property "Instance" oder Methode "get_Instance",
    // in Klasse oder Basisklassen. Gibt den Getter-MonoMethod* zurück.
    inline Result<_internal::MonoMethod*> get_instance_accessor(
        const std::string& ns, const std::string& class_name, const std::string& assembly_name) {
        ThreadScope scope;
        if (class_name.empty() || assembly_name.empty()) return { MonoStatus::InvalidArgs, nullptr };

        auto c = find_class(ns, class_name, assembly_name);
        if (!c) return { c.status, nullptr };

        for (_internal::MonoClass* k = c.value; k; k = _internal::mono_class_get_parent ? _internal::mono_class_get_parent(k) : nullptr) {
            if (_internal::mono_class_get_property_from_name && _internal::mono_property_get_get_method) {
                _internal::MonoProperty* prop = _internal::mono_class_get_property_from_name(k, "Instance");
                if (prop) {
                    _internal::MonoMethod* getter = _internal::mono_property_get_get_method(prop);
                    if (getter) return { MonoStatus::OK, getter };
                }
            }
            for (int n = 0; n <= 16; ++n) {
                _internal::MonoMethod* mi = _internal::mono_class_get_method_from_name(k, "get_Instance", n);
                if (mi) return { MonoStatus::OK, mi };
            }
        }
        return { MonoStatus::MethodNotFound, nullptr };
    }

} // namespace mono
