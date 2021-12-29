// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/risk/risk.h"

#include "src/execution/isolate-inl.h"

#include "src/risk/hs-helper.h"

namespace v8 {
namespace internal {

MaybeHandle<Object> Risk::SetPattern(Isolate* isolate, Handle<String> patterns) {
  DisallowHeapAllocation no_gc;
  int len = 0;
  auto tmp = patterns->ToCString(DISALLOW_NULLS, FAST_STRING_TRAVERSAL, &len);
  std::string pats;
  pats.assign(&tmp[0], len);
  int32_t id = risk::HsHelper::instance().SetPattern(pats);
  Handle<Object> result = isolate->factory()->NewNumberFromInt(id);
  return result;
}

MaybeHandle<Object> Risk::TestPattern(Isolate* isolate, Handle<Object> patId,
                                       Handle<String> haystack) {
  int32_t id, ret;
  bool bOK = patId->ToInt32(&id);
  if (bOK) {
    int len = 0;
    auto tmp = haystack->ToCString(DISALLOW_NULLS, FAST_STRING_TRAVERSAL, &len);
    std::string hay;
    hay.assign(&tmp[0], len);
    ret = risk::HsHelper::instance().Test(id, hay);
  } else {
    ret = -100;
  }
  Handle<Object> result = isolate->factory()->NewNumberFromInt(ret);
  return result;
}

MaybeHandle<Object> Risk::GetLastError(Isolate* isolate) {
  const std::string& errMsg = risk::HsHelper::instance().GetLastError();
  return isolate->factory()->NewStringFromUtf8(CStrVector(errMsg.c_str()));
}

}  // namespace internal
}  // namespace v8s
