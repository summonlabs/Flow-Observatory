// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Shared-library export/import decoration. The runtime is usable as a static
// library (default), a shared library, or vendored source. All three modes use
// the same headers.

#ifndef FLOWOBS_EXPORT_HPP
#define FLOWOBS_EXPORT_HPP

#if defined(_WIN32) && defined(FLOWOBS_SHARED)
#if defined(FLOWOBS_BUILDING_LIBRARY)
#define FLOWOBS_API __declspec(dllexport)
#else
#define FLOWOBS_API __declspec(dllimport)
#endif
#else
#define FLOWOBS_API
#endif

// Marks a symbol that is part of the supported public surface. Anything not
// marked FLOWOBS_API lives in an installed header but is an implementation
// detail and may change within a minor release.
#define FLOWOBS_PUBLIC FLOWOBS_API

// Internal linkage helper used by the implementation only.
#define FLOWOBS_INTERNAL

#endif  // FLOWOBS_EXPORT_HPP
