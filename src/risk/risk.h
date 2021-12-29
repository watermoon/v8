// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_RISK_RISK_H_
#define V8_RISK_RISK_H_

#include "src/utils/allocation.h"
#include "src/handles/maybe-handles.h"
#include "src/objects/objects.h"
#include "src/risk/risk.h"

namespace v8 {
namespace internal {

class Risk : public AllStatic {
 public:
  // SetPatterns
  static MaybeHandle<Object> RiskSetPattern(Isolate* isolate, Handle<String> patterns) {
    return SetPattern(isolate, patterns);
  }

  static MaybeHandle<Object> RiskTestPattern(Isolate* isolate, Handle<Object> patId, Handle<String> haystack) {
    return TestPattern(isolate, patId, haystack);
  }

  static MaybeHandle<Object> RiskGetLastError(Isolate* isolate) {
      return GetLastError(isolate);
  }

 private:
  static MaybeHandle<Object> SetPattern(Isolate* isolate, Handle<String> patterns);
  static MaybeHandle<Object> TestPattern(Isolate* isolate, Handle<Object> patId, Handle<String> haystack);
  static MaybeHandle<Object> GetLastError(Isolate* isolate);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_RISK_RISK_H_