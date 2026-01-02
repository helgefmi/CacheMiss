#pragma once

#include <cstddef>

// Run the UCI protocol loop
// hash_mb: size of hash table in megabytes
void uci_loop(size_t hash_mb = 512);
