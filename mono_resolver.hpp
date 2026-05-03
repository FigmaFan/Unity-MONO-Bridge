#pragma once
// mono_resolver.hpp (v1.0): lazy export binding, RAII ThreadScope, Result<T> statuses, array helpers, enum reflection, unity engine utils

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
        struct _MonoImage;
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
        using MonoImage = _MonoImage;
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
        inline std::once_flag g_mono_module_once;
        inline std::unordered_map<std::string, MonoAssembly*> g_assembly_cache;
        inline std::mutex g_cache_mtx;

        inline std::unordered_map<std::string, MonoImage*> g_image_cache;
        inline std::mutex g_image_cache_mtx;

        // mono_runtime_invoke: value types need &arg in argv; reference types need the pointer value (not &ptr)
        template <typename T>
        inline void* mono_arg_ptr(T& arg) noexcept {
            if constexpr (std::is_pointer_v<std::remove_reference_t<T>>) {
                return reinterpret_cast<void*>(arg);
            }
            else {
                return reinterpret_cast<void*>(&arg);
            }
        }

        inline Result<HMODULE> ensure_mono_module() {
            std::call_once(g_mono_module_once, [] {
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
            });

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

        inline MonoDomain* (__cdecl* mono_get_root_domain)() = nullptr;
        inline MonoThread* (__cdecl* mono_thread_attach)(MonoDomain*) = nullptr;
        inline void(__cdecl* mono_thread_detach)(MonoThread*) = nullptr;
        inline MonoThread* (__cdecl* mono_thread_current)() = nullptr;

        inline MonoAssembly* (__cdecl* mono_domain_assembly_open)(MonoDomain*, const char*) = nullptr;
        inline MonoImage* (__cdecl* mono_assembly_get_image)(MonoAssembly*) = nullptr;

        // Unity loads images by logical name; mono_image_loaded is preferred over mono_domain_assembly_open
        inline MonoImage* (__cdecl* mono_image_loaded)(const char*) = nullptr;
        inline const char* (__cdecl* mono_image_get_name)(MonoImage*) = nullptr;
        inline const char* (__cdecl* mono_image_get_filename)(MonoImage*) = nullptr;

        inline MonoClass* (__cdecl* mono_class_from_name)(MonoImage*, const char*, const char*) = nullptr;
        inline MonoMethod* (__cdecl* mono_class_get_method_from_name)(MonoClass*, const char*, int) = nullptr;
        inline MonoClassField* (__cdecl* mono_class_get_field_from_name)(MonoClass*, const char*) = nullptr;
        inline MonoClassField* (__cdecl* mono_class_get_fields)(MonoClass*, void**) = nullptr;
        inline const char* (__cdecl* mono_field_get_name)(MonoClassField*) = nullptr;

        inline void(__cdecl* mono_field_get_value)(MonoObject*, MonoClassField*, void*) = nullptr;
        inline void(__cdecl* mono_field_set_value)(MonoObject*, MonoClassField*, void*) = nullptr;

        inline MonoVTable* (__cdecl* mono_class_vtable)(MonoDomain*, MonoClass*) = nullptr;
        inline void(__cdecl* mono_field_static_get_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
        inline void(__cdecl* mono_field_static_set_value)(MonoVTable*, MonoClassField*, void*) = nullptr;

        inline MonoObject* (__cdecl* mono_field_get_value_object)(MonoDomain*, MonoClassField*, MonoObject*) = nullptr;

        inline MonoObject* (__cdecl* mono_object_new)(MonoDomain*, MonoClass*) = nullptr;
        inline MonoObject* (__cdecl* mono_runtime_invoke)(MonoMethod*, void*, void**, MonoObject**) = nullptr;
        inline void* (__cdecl* mono_object_unbox)(MonoObject*) = nullptr;

        inline MonoString* (__cdecl* mono_string_new)(MonoDomain*, const char*) = nullptr;
        inline char* (__cdecl* mono_string_to_utf8)(MonoString*) = nullptr;
        inline void(__cdecl* mono_free)(void*) = nullptr;

        inline MonoClass* (__cdecl* mono_get_enum_class)() = nullptr;
        inline mono::_internal::MonoType* (__cdecl* mono_field_get_type)(MonoClassField*) = nullptr;
        inline mono::_internal::MonoClass* (__cdecl* mono_type_get_class)(MonoType*) = nullptr;
        inline mono::_internal::MonoDomain* (__cdecl* mono_object_get_domain)(MonoObject*) = nullptr;

        inline uintptr_t(__cdecl* mono_array_length)(MonoArray*) = nullptr;
        inline void* (__cdecl* mono_array_addr_with_size)(MonoArray*, int, uintptr_t) = nullptr;

        inline mono::_internal::MonoClass* (__cdecl* mono_object_get_class)(MonoObject*) = nullptr;
        inline bool(__cdecl* mono_class_is_enum)(MonoClass*) = nullptr;

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

            if (auto s = bind_req(mono_get_root_domain, "mono_get_root_domain", MonoStatus::Missing_get_root_domain); s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_thread_attach, "mono_thread_attach", MonoStatus::Missing_thread_attach);   s != MonoStatus::OK) return s;
            bind_opt(mono_thread_detach, "mono_thread_detach");
            bind_opt(mono_thread_current, "mono_thread_current");

            if (auto s = bind_req(mono_domain_assembly_open, "mono_domain_assembly_open", MonoStatus::Missing_domain_assembly_open); s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_assembly_get_image, "mono_assembly_get_image", MonoStatus::Missing_assembly_get_image);   s != MonoStatus::OK) return s;

            bind_opt(mono_image_loaded, "mono_image_loaded");
            bind_opt(mono_image_get_name, "mono_image_get_name");
            bind_opt(mono_image_get_filename, "mono_image_get_filename");

            if (auto s = bind_req(mono_class_from_name, "mono_class_from_name", MonoStatus::Missing_class_from_name);            s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_class_get_method_from_name, "mono_class_get_method_from_name", MonoStatus::Missing_class_get_method_from_name); s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_class_get_field_from_name, "mono_class_get_field_from_name", MonoStatus::Missing_class_get_field_from_name);  s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_class_get_fields, "mono_class_get_fields", MonoStatus::Missing_class_get_field_from_name);    s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_field_get_name, "mono_field_get_name", MonoStatus::Missing_class_get_field_from_name);    s != MonoStatus::OK) return s;

            if (auto s = bind_req(mono_field_get_value, "mono_field_get_value", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_field_set_value, "mono_field_set_value", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;

            if (auto s = bind_req(mono_class_vtable, "mono_class_vtable", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_field_static_get_value, "mono_field_static_get_value", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_field_static_set_value, "mono_field_static_set_value", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;

            if (auto s = bind_req(mono_field_get_value_object, "mono_field_get_value_object", MonoStatus::Missing_field_get_set);                s != MonoStatus::OK) return s;

            if (auto s = bind_req(mono_object_new, "mono_object_new", MonoStatus::GetProcAddressFailed);     s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_runtime_invoke, "mono_runtime_invoke", MonoStatus::Missing_runtime_invoke);   s != MonoStatus::OK) return s;
            if (auto s = bind_req(mono_object_unbox, "mono_object_unbox", MonoStatus::Missing_object_unbox);     s != MonoStatus::OK) return s;

            bind_opt(mono_string_new, "mono_string_new");
            bind_opt(mono_string_to_utf8, "mono_string_to_utf8");
            bind_opt(mono_free, "mono_free");

            bind_opt(mono_array_length, "mono_array_length");
            bind_opt(mono_array_addr_with_size, "mono_array_addr_with_size");

            bind_opt(mono_get_enum_class, "mono_get_enum_class");
            bind_opt(mono_field_get_type, "mono_field_get_type");
            bind_opt(mono_type_get_class, "mono_type_get_class");
            bind_opt(mono_object_get_domain, "mono_object_get_domain");
            bind_opt(mono_object_get_class, "mono_object_get_class");
            bind_opt(mono_class_is_enum, "mono_class_is_enum");

            return MonoStatus::OK;
        }

        inline Result<MonoAssembly*> find_assembly(std::string_view assembly_name) {
            if (assembly_name.empty()) return { MonoStatus::InvalidArgs, nullptr };
            if (auto s = ensure_exports(); s != MonoStatus::OK) return { s, nullptr };

            {
                std::scoped_lock lk(g_cache_mtx);
                if (auto it = g_assembly_cache.find(std::string(assembly_name)); it != g_assembly_cache.end())
                    return { MonoStatus::OK, it->second };
            }

            auto* domain = mono_get_root_domain ? mono_get_root_domain() : nullptr;
            if (!domain) return { MonoStatus::DomainUnavailable, nullptr };

            MonoAssembly* ass = mono_domain_assembly_open(domain, std::string(assembly_name).c_str());
            if (!ass) return { MonoStatus::AssemblyNotFound, nullptr };

            {
                std::scoped_lock lk(g_cache_mtx);
                g_assembly_cache.emplace(std::string(assembly_name), ass);
            }
            return { MonoStatus::OK, ass };
        }

        // Image cache: mono_image_loaded with/without ".dll", then find_assembly + mono_assembly_get_image
        inline Result<MonoImage*> find_image(std::string_view image_name) {
            if (image_name.empty()) return { MonoStatus::InvalidArgs, nullptr };
            if (auto s = ensure_exports(); s != MonoStatus::OK) return { s, nullptr };

            const std::string key(image_name);

            {
                std::scoped_lock lk(g_image_cache_mtx);
                if (auto it = g_image_cache.find(key); it != g_image_cache.end())
                    return { MonoStatus::OK, it->second };
            }

            MonoImage* img = nullptr;

            if (mono_image_loaded) {
                img = mono_image_loaded(key.c_str());

                if (!img) {
                    constexpr std::string_view kDll = ".dll";
                    std::string alt;
                    if (key.size() > kDll.size() &&
                        std::string_view(key).substr(key.size() - kDll.size()) == kDll) {
                        alt.assign(key, 0, key.size() - kDll.size());
                    }
                    else {
                        alt.reserve(key.size() + kDll.size());
                        alt.assign(key);
                        alt.append(kDll);
                    }
                    img = mono_image_loaded(alt.c_str());
                }
            }

            if (!img) {
                auto ass = find_assembly(image_name);
                if (ass && mono_assembly_get_image) {
                    img = mono_assembly_get_image(ass.value);
                }
                else if (!ass) {
                    return { ass.status, nullptr };
                }
            }

            if (!img) return { MonoStatus::AssemblyNotFound, nullptr };

            {
                std::scoped_lock lk(g_image_cache_mtx);
                g_image_cache.emplace(key, img);
            }
            return { MonoStatus::OK, img };
        }

    } // _internal

    // ThreadScope: attach once per thread (mono_thread_attach is idempotent). Never call mono_thread_current
    // on a never-attached worker (TLS can crash). Intentionally never detach (hooks may run later on same thread)
    struct ThreadScope {
        inline static thread_local bool s_attached_this_thread{ false };

        ThreadScope() noexcept {
            if (s_attached_this_thread) return;
            if (_internal::ensure_exports() != MonoStatus::OK) return;
            if (!_internal::mono_get_root_domain || !_internal::mono_thread_attach) return;

            auto* dom = _internal::mono_get_root_domain();
            if (!dom) return;

            auto* th = _internal::mono_thread_attach(dom);
            if (th) s_attached_this_thread = true;
        }

        ~ThreadScope() noexcept = default;
    };

    // No explicit init(); module and exports resolve lazily (call_once / null checks)
    inline void cleanup() {
        {
            std::scoped_lock lk(_internal::g_cache_mtx);
            _internal::g_assembly_cache.clear();
        }
        {
            std::scoped_lock lk(_internal::g_image_cache_mtx);
            _internal::g_image_cache.clear();
        }
    }

    inline Result<_internal::MonoClass*>
        find_class(const std::string& ns, const std::string& class_name, const std::string& assembly_name) {
        ThreadScope scope;
        if (ns.empty() || class_name.empty() || assembly_name.empty())
            return { MonoStatus::InvalidArgs, nullptr };

        auto img_r = _internal::find_image(assembly_name);
        if (!img_r) return { img_r.status, nullptr };

        auto* klass = _internal::mono_class_from_name(img_r.value, ns.c_str(), class_name.c_str());
        if (!klass) return { MonoStatus::ClassNotFound, nullptr };

        return { MonoStatus::OK, klass };
    }

    inline Result<_internal::MonoMethod*>
        get_method(const std::string& ns, const std::string& class_name, const std::string& method_name,
            const std::string& assembly_name, std::optional<int> param_count = std::nullopt) {
        ThreadScope scope;
        if (ns.empty() || class_name.empty() || method_name.empty() || assembly_name.empty())
            return { MonoStatus::InvalidArgs, nullptr };

        auto c = find_class(ns, class_name, assembly_name);
        if (!c) return { c.status, nullptr };

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

        if (!mi) return { MonoStatus::MethodNotFound, nullptr };
        return { MonoStatus::OK, mi };
    }

    inline Result<_internal::MonoClassField*>
        get_field(const std::string& ns, const std::string& class_name,
            const std::string& field_name, const std::string& assembly_name) {
        ThreadScope scope;
        if (ns.empty() || class_name.empty() || field_name.empty() || assembly_name.empty())
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

            void* argv[sizeof...(CtorArgs)] = { _internal::mono_arg_ptr(ctor_args)... };
            _internal::MonoObject* exc = nullptr;
            _internal::MonoObject* res = _internal::mono_runtime_invoke(mi_ctor.value,
                reinterpret_cast<void*>(r.value),
                argv, &exc);
            (void)res;
            if (exc) return { MonoStatus::InvokeException, nullptr };
        }
        return r;
    }

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
        }
        else {
            void* argv[sizeof...(Args)] = { _internal::mono_arg_ptr(args)... };
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

    namespace Type {

        namespace _detail {
            inline Result<mono::_internal::MonoMethod*> resolve_get_type_method() {
                static constexpr const char* kAssemblyCandidates[] = {
                    "mscorlib.dll",
                    "mscorlib",
                };
                MonoStatus last = MonoStatus::MethodNotFound;
                for (auto* asm_name : kAssemblyCandidates) {
                    auto m = mono::get_method("System", "Type", "GetType", asm_name, 1);
                    if (m) return m;
                    last = m.status;
                }
                return { last, nullptr };
            }
        }

        // System.Type.GetType(string) via existing MonoString*
        inline Result<void*> GetType(void* type_name_mono_string) {
            ThreadScope scope;
            if (!type_name_mono_string) return { MonoStatus::InvalidArgs, nullptr };

            auto method = _detail::resolve_get_type_method();
            if (!method) return { method.status, nullptr };

            auto result = call_function<void*>(method.value, nullptr, type_name_mono_string);
            if (!result) return { result.status, nullptr };
            if (!result.value) return { MonoStatus::ClassNotFound, nullptr };
            return { MonoStatus::OK, result.value };
        }

        inline Result<void*> GetType(const std::string& type_name) {
            ThreadScope scope;
            if (type_name.empty()) return { MonoStatus::InvalidArgs, nullptr };

            auto mono_str = String::CreateNewString(type_name);
            if (!mono_str) return { mono_str.status, nullptr };

            return GetType(mono_str.value);
        }

    } // namespace Type

    namespace UnityEngine {

        namespace _detail {
            inline Result<mono::_internal::MonoMethod*>
                resolve_object_method(const char* method_name, int param_count) {
                static constexpr const char* kAssemblyCandidates[] = {
                    "UnityEngine.CoreModule.dll",
                    "UnityEngine.CoreModule"
                };
                MonoStatus last = MonoStatus::MethodNotFound;
                for (auto* asm_name : kAssemblyCandidates) {
                    auto m = mono::get_method("UnityEngine", "Object", method_name, asm_name, param_count);
                    if (m) return m;
                    last = m.status;
                }
                return { last, nullptr };
            }

            inline Result<mono::_internal::MonoMethod*>
                resolve_find_object_of_type_method(int param_count) {
                return resolve_object_method("FindObjectOfType", param_count);
            }

            inline Result<mono::_internal::MonoMethod*>
                resolve_find_objects_of_type_method(int param_count) {
                return resolve_object_method("FindObjectsOfType", param_count);
            }
        }

        // UnityEngine.Object.FindObjectOfType; no match => { OK, nullptr }
        inline Result<void*> FindObjectOfType(void* type_instance, bool include_inactive = false) {
            ThreadScope scope;
            if (!type_instance) return { MonoStatus::InvalidArgs, nullptr };

            auto method = _detail::resolve_find_object_of_type_method(2);
            if (!method) {
                auto m1 = _detail::resolve_find_object_of_type_method(1);
                if (!m1) {
                    return { method.status, nullptr };
                }
                auto r = call_function<void*>(m1.value, nullptr, type_instance);
                if (!r) return { r.status, nullptr };
                return { MonoStatus::OK, r.value };
            }

            auto result = call_function<void*>(method.value, nullptr,
                type_instance, include_inactive);
            if (!result) return { result.status, nullptr };
            return { MonoStatus::OK, result.value };
        }

        inline Result<void*> FindObjectOfType(const std::string& type_name,
            bool include_inactive = false) {
            ThreadScope scope;
            if (type_name.empty()) return { MonoStatus::InvalidArgs, nullptr };

            auto type = mono::Type::GetType(type_name);
            if (!type) {
                return { type.status, nullptr };
            }

            return FindObjectOfType(type.value, include_inactive);
        }

        // UnityEngine.Object.FindObjectsOfType; empty array is valid.
        inline Result<void*> FindObjectsOfType(void* type_instance, bool include_inactive = false) {
            ThreadScope scope;
            if (!type_instance) return { MonoStatus::InvalidArgs, nullptr };

            auto method = _detail::resolve_find_objects_of_type_method(2);
            if (!method) {
                auto m1 = _detail::resolve_find_objects_of_type_method(1);
                if (!m1) {
                    return { method.status, nullptr };
                }
                auto r = call_function<void*>(m1.value, nullptr, type_instance);
                if (!r) return { r.status, nullptr };
                return { MonoStatus::OK, r.value };
            }

            auto result = call_function<void*>(method.value, nullptr,
                type_instance, include_inactive);
            if (!result) return { result.status, nullptr };
            return { MonoStatus::OK, result.value };
        }

        inline Result<void*> FindObjectsOfType(const std::string& type_name,
            bool include_inactive = false) {
            ThreadScope scope;
            if (type_name.empty()) return { MonoStatus::InvalidArgs, nullptr };

            auto type = mono::Type::GetType(type_name);
            if (!type) {
                return { type.status, nullptr };
            }

            return FindObjectsOfType(type.value, include_inactive);
        }

        // UnityEngine.Object.name (CIL get_name)
        inline Result<std::string> get_name(void* unity_object) {
            ThreadScope scope;
            if (!unity_object) return { MonoStatus::InvalidArgs, {} };

            auto method = _detail::resolve_object_method("get_name", 0);
            if (!method) return { method.status, {} };

            auto result = call_function<void*>(method.value, unity_object);
            if (!result) return { result.status, {} };
            if (!result.value) return { MonoStatus::OK, {} };

            return { MonoStatus::OK, mono::String::convert_to_std_string(result.value) };
        }

    } // namespace UnityEngine

    namespace Array {

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

    } // namespace Array

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

} // namespace mono
