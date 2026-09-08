/*
 * Project-owned signing trust configuration.
 *
 * Release builds intentionally ship with project trust disabled until a
 * separately reviewed generated configuration is supplied at build time.
 * Define SL_PROJECT_TRUST_CONFIG_HEADER to a quoted include path naming the
 * generated file emitted by tools/project-signing.ps1. The generated file
 * contains only the public P-256 key, key ID, and exact accepted release ID.
 * It must never contain a private key.
 */

#pragma once

#define SL_PROJECT_TRUST_CONFIGURED 0
#define SL_PROJECT_TRUST_KEY_ID 0u
#define SL_PROJECT_TRUST_RELEASE_ID ""

