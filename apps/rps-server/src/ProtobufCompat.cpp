#include <google/protobuf/repeated_ptr_field.h>

#if defined(_MSC_VER) && defined(__clang__)
namespace google::protobuf::internal {

// clang-cl (MSVC ABI) can emit a restrict-qualified mangling for this symbol
// that differs from protobuf binaries built with cl.exe. Emit the clang-side
// instantiation locally so links remain stable across compiler choices.
template void memswap<ArenaOffsetHelper<RepeatedPtrFieldBase>::value>(
    char* __restrict a, char* __restrict b);

} // namespace google::protobuf::internal
#endif
