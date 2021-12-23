#include "src/api/api-inl.h"
#include "src/codegen/code-factory.h"
#include "src/codegen/compiler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/compiler/common-operator.h"
#include "src/compiler/graph.h"
#include "src/compiler/linkage.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/node.h"
#include "src/compiler/operator.h"
#include "src/compiler/pipeline.h"
#include "src/compiler/schedule.h"
#include "src/objects/objects-inl.h"
#include "src/parsing/parse-info.h"
#include "src/zone/zone.h"
#include "test/cctest/cctest.h"

namespace v8 {
namespace internal {
namespace compiler {

TEST(MathIsHeapNumber42) {
    // HandleAndZoneScope scope;
    // Isolate* isolate = scope.main_isolate();
    // Heap* heap = isolate->heap();
    // Zone* zone = scope.main_zone();

    // StubTester tester(isolate, zone, Builtins::kMathIs42);
    // Handle<Object> result = tester.Call(Handle<Smi>(Smi::FromInt(0), isolate));
    // CHECK(result->BooleanValue());
    // Isolate* isolate = CcTest::InitIsolateOnce();
    // Zone zone(isolate->allocator(), ZONE_NAME);
    // Callable callable =
    //     Builtins::CallableFor(isolate, Builtins::kMathIs42);

    // OptimizedCompilationInfo info(ArrayVector("abc"), &zone,
    //                             CodeKind::FOR_TESTING);
    // auto call_descriptor = Linkage::GetStubCallDescriptor(
    //     &zone, callable.descriptor(), 0, CallDescriptor::kNoFlags,
    //     Operator::kNoProperties);
    // CHECK(call_descriptor);
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
