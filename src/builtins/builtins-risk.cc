#include "src/builtins/builtins-utils-inl.h"
#include "src/builtins/builtins.h"
#include "src/codegen/code-factory.h"
#include "src/codegen/compiler.h"
#include "src/logging/counters.h"
#include "src/objects/objects-inl.h"

#include "src/risk/risk.h"

namespace v8 {
namespace internal {
  void hs_callback(int a) {
    PrintF("### demo_callback| a=%d", a);
  }

  int hs_cppfunc(const char* msg, int id, int* res) {
    // mksnapshot 的时候执行
    PrintF("### demo_cppfunc| msg=%s id=%d", msg, id);
    for (int i = 0; i < 3; ++i) {
        hs_callback(id * 10 + i);
    }
    *res = 3322;
    return id * 10;
  }

  BUILTIN(RiskSetPattern) {
    HandleScope scope(isolate);
    Handle<String> pattern;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, pattern,
      Object::ToString(isolate, args.atOrUndefined(isolate, 1)));

    RETURN_RESULT_OR_FAILURE(isolate, Risk::RiskSetPattern(isolate, pattern));
  }

  BUILTIN(RiskTestPattern) {
    HandleScope scope(isolate);
    Handle<String> haystack;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, haystack,
      Object::ToString(isolate, args.atOrUndefined(isolate, 2)));
    Handle<Object> patId = args.atOrUndefined(isolate, 1);
  
    RETURN_RESULT_OR_FAILURE(isolate, Risk::RiskTestPattern(isolate, patId, haystack));
  }

  BUILTIN(RiskGetLastError) {
    HandleScope scope(isolate);
    RETURN_RESULT_OR_FAILURE(isolate, Risk::RiskGetLastError(isolate));
  }
}  // namespace internal
}  // namespace v8
