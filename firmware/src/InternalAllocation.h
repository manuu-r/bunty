#pragma once

namespace memory {
// Initialize once from setup, before starting application workers.
void beginAllocationScopes();

// Some precompiled IDF drivers allocate ISR-visible control structures with
// plain malloc/calloc. Force those calls to internal RAM only during setup on
// the calling task. Other tasks retain the framework's allocation policy.
class InternalAllocationScope {
 public:
  InternalAllocationScope();
  ~InternalAllocationScope();
  InternalAllocationScope(const InternalAllocationScope &) = delete;
  InternalAllocationScope &operator=(const InternalAllocationScope &) = delete;
};
}
