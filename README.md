# Unity Mono Bridge

[![Stars][stars-shield]][stars-url] [![Forks][forks-shield]][forks-url] [![Contributors][contributors-shield]][contributors-url] [![License][license-shield]][license-url]

## About The Project

**Unity Mono Bridge** is a small, header-only C++ library that binds to the Unity **Mono** runtime at runtime via export resolution: assemblies, classes, methods, and fields are resolved dynamically — no hardcoded offsets. All public operations return `mono::Result<T>` with an explicit `MonoStatus`; the calling thread is attached to the Mono domain when needed through `ThreadScope`.

### Features

- **Lazy binding** of the Mono DLL (`mono-2.0-bdwgc.dll`, `mono-2.0-sgen.dll`, `mono.dll`), including a short wait until the module is loaded
- **Export resolution** with thread-safe assembly and image caches
- **Class and method resolution** by namespace, class name, method name, and assembly (optional: parameter count for overloads)
- **Invocations** via `mono_runtime_invoke` with a typed template API (`call_function<Ret>`)
- **Fields** (instance and static): read/write primitive values
- **Object creation** with optional constructor invocation
- **Strings**: create `System.String` from `std::string` and convert to UTF-8
- **1D arrays**: length and element access via `mono_array_length` / `mono_array_addr_with_size`
- **Enum reflection**: cached entries, name ↔ value, index map
- **Unity helpers**: `System.Type.GetType`, `UnityEngine.Object.FindObjectOfType` / `FindObjectsOfType`, `get_name`

## Prerequisites

- Windows SDK
- C++17 compiler (MSVC recommended)
- Unity application using the **Mono** backend

## Built With

- C++17
- Win32 (`Windows.h`)
- Mono runtime exports (see header)

## Installation

1) Include the header:

```cpp
#include "mono_resolver.hpp"
```

2) **No** separate `init()` call: module and export resolution happen **lazily** on the first API use (`std::call_once` for the module, `ensure_exports()` for function pointers).

3) Optionally clear caches when unloading a DLL:

```cpp
mono::cleanup();
```

---

## Usage

### `Result<T>` and error handling

Every resolver/caller returns `mono::Result<T>`:

```cpp
auto mi = mono::get_method("UnityEngine", "Object", "FindObjectOfType", "UnityEngine.CoreModule.dll", 2);
if (!mi) {
    // Inspect mi.status, e.g. log to_string(mi.status)
}
auto* method = mi.value; // only if mi == true (internal MonoMethod*)
```

For `void` returns you get `Result<void>` (only `status`, no `value`).

---

### Classes, methods, and fields

```cpp
auto klass = mono::find_class("UnityEngine", "GameObject", "UnityEngine.CoreModule.dll");
if (!klass) { /* check klass.status */ }

// 5th parameter optional: std::optional<int>{n} for an exact overload
auto m = mono::get_method("UnityEngine", "Transform", "get_position", "UnityEngine.CoreModule.dll");
if (!m) { /* check m.status */ }

auto f = mono::get_field("MyNs", "MyClass", "myField", "Assembly-CSharp.dll");
if (!f) { /* check f.status */ }
```

Without `param_count`, `get_method` internally tries parameter counts **0 through 16**; for ambiguous overloads, pass the count explicitly.

---

### Managed calls (`mono_runtime_invoke`)

`call_function<Ret>(method, instance, args...)` builds `argv` the way Mono expects: **value types** as the address of the argument, **reference types** as the pointer value (no `&` on the handle).

**Static method (instance `nullptr`):**

```cpp
auto mi = mono::get_method("System", "Math", "Abs", "mscorlib.dll", 1);
if (mi) {
    int x = -5;
    auto r = mono::call_function<int>(mi.value, nullptr, x);
    if (r) {
        int y = r.value;
    }
}
```

**Instance method:**

```cpp
void* pObj = /* MonoObject* / managed instance */;
auto mi = mono::get_method("MyNs", "MyClass", "DoWork", "Assembly-CSharp.dll", 1);
if (mi) {
    float arg = 1.0f;
    auto st = mono::call_function<void>(mi.value, pObj, arg);
    if (!st) { /* st.status, possibly InvokeException */ }
}
```

**Return values:** pointer types are returned as `MonoObject*` / `void*`; value types are copied into `Ret` via `mono_object_unbox` and `memcpy`.

---

### Fields: instance and static

**Instance:**

```cpp
auto val = mono::get_object_field_value<int>(
    instance, "MyNs", "MyClass", "health", "Assembly-CSharp.dll");
if (val) { int h = val.value; }

auto st = mono::set_object_field_value<int>(
    instance, "MyNs", "MyClass", "health", 100, "Assembly-CSharp.dll");
```

**Static:**

```cpp
auto cls = mono::find_class("MyNs", "MyClass", "Assembly-CSharp.dll");
if (cls) {
    auto s = mono::get_static_field_value<void*>(cls.value, "singletonInstance");
    if (s) { void* p = s.value; }
}
```

---

### Creating objects

```cpp
// Allocation only (class .ctor can be invoked separately)
auto obj = mono::create_object(klass);

// Namespace/class/assembly with optional constructor arguments
auto nameStr = mono::String::CreateNewString("MyGO");
if (nameStr) {
    auto go = mono::create_object<void*>(
        "UnityEngine", "GameObject", "UnityEngine.CoreModule.dll",
        nameStr.value);  // e.g. ctor(string)
}
```

If constructor arguments are provided, `.ctor` is invoked via `mono_runtime_invoke`; a managed exception yields `MonoStatus::InvokeException`.

---

### Strings

```cpp
auto s = mono::String::CreateNewString("hello");
if (s) {
    void* monoStr = s.value;
}

std::string cpp = mono::String::convert_to_std_string(monoStr);
```

`mono_string_to_utf8` / `mono_free` are bound lazily; if those exports are missing, `convert_to_std_string` returns an empty string.

---

### Arrays (1D)

```cpp
auto len = mono::Array::array_get_length_1d(arrPtr);
if (len) {
    for (int i = 0; i < len.value; ++i) {
        auto elem = mono::Array::array_get_element_1d<void*>(arrPtr, static_cast<uintptr_t>(i));
        if (elem) { void* e = elem.value; }
    }
}
```

`array_get_element_1d` copies `sizeof(Ret)` bytes from the array element — for reference types, typically `void*` / pointer size.

---

### `System.Type` and UnityEngine

```cpp
// Type by assembly-qualified name (as in C#)
auto t = mono::Type::GetType("UnityEngine.Camera, UnityEngine.CoreModule");
if (t) {
    auto cam = mono::UnityEngine::FindObjectOfType(t.value, false);
    if (cam && cam.value) { /* first Camera */ }
}

// Shorthand: type name as string
auto cam2 = mono::UnityEngine::FindObjectOfType("UnityEngine.Camera, UnityEngine.CoreModule");

auto name = mono::UnityEngine::get_name(gameObjectPtr);
if (name) { std::string n = name.value; }
```

`FindObjectOfType` / `FindObjectsOfType` try the two-parameter overload (`Type`, `bool includeInactive`) first, then the one-parameter overload. No match for `FindObjectOfType` is **not** an error: `{ OK, nullptr }`.

---

### Enums

```cpp
auto entries = mono::Enum::get_entries("UnityEngine", "Space", "UnityEngine.CoreModule.dll");
if (entries) {
    for (const auto& e : *entries.value) {
        // e.value, e.name
    }
}

auto n = mono::Enum::name_of("UnityEngine", "Space", "UnityEngine.CoreModule.dll", 0);
auto v = mono::Enum::value_of("UnityEngine", "Space", "UnityEngine.CoreModule.dll", "World");

auto map = mono::Enum::index_map("UnityEngine", "Space", "UnityEngine.CoreModule.dll");
if (map) {
    int idx = map.value.index_of(1);
}
```

---

## Threading: `ThreadScope`

Many APIs construct a `ThreadScope` internally: one `mono_thread_attach` per thread (`thread_local`). There is **intentionally no** `mono_thread_detach` in the destructor so later hooks on the same thread do not crash. Avoid calling `mono_thread_current` on a “fresh” worker thread that has never been attached (TLS risk).

---

## Status and logging

Use `to_string(result.status)` with your own logging or diagnostics.

**Notable statuses**

- `MonoModuleNotFound`, `GetProcAddressFailed`
- `Missing_*` — a required export is missing from the loaded Mono DLL
- `DomainUnavailable`, `AssemblyNotFound`, `ImageUnavailable`
- `ClassNotFound`, `MethodNotFound`, `FieldNotFound`
- `InvalidArgs`, `ThreadAttachUnavailable`, `InvokeException`
- `OK`

---

## Notes

- This library is **not** compatible with IL2CPP (`GameAssembly.dll`) — use the separate **IL2CPP Bridge** project for that

---

## Contributing

Contributions are welcome. Please include a minimal repro and test against the current header.

## License

Public domain / Unlicense — see [LICENSE](LICENSE).

[license-shield]: https://img.shields.io/github/license/FigmaFan/Unity-MONO-Bridge.svg?style=for-the-badge
[license-url]: https://github.com/FigmaFan/Unity-MONO-Bridge/blob/master/LICENSE
[stars-shield]: https://img.shields.io/github/stars/FigmaFan/Unity-MONO-Bridge.svg?style=for-the-badge
[stars-url]: https://github.com/FigmaFan/your_repo/stargazers
[forks-shield]: https://img.shields.io/github/forks/FigmaFan/Unity-MONO-Bridge.svg?style=for-the-badge
[forks-url]: https://github.com/FigmaFan/Unity-MONO-Bridge/network/members
[contributors-shield]: https://img.shields.io/github/contributors/FigmaFan/Unity-MONO-Bridge.svg?style=for-the-badge
[contributors-url]: https://github.com/FigmaFan/Unity-MONO-Bridge/graphs/contributors
