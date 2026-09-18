// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef BANDWIDTH_BROKER_EXPORT_HPP
#define BANDWIDTH_BROKER_EXPORT_HPP

#if defined(_WIN32) && defined(BB_SHARED_BUILD)
#if defined(BB_BUILDING_LIBRARY)
#define BB_API __declspec(dllexport)
#else
#define BB_API __declspec(dllimport)
#endif
#else
#define BB_API
#endif

// Marks APIs that may be extended in a future minor release without notice.
#define BB_EXPERIMENTAL

#endif  // BANDWIDTH_BROKER_EXPORT_HPP
