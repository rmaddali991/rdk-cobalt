// Copyright 2018 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "starboard/common/log.h"

#include "third_party/starboard/wpe/shared/application_wpe.h"

#include <essos-app.h>

extern "C" EGLDisplay __real_eglGetDisplay(EGLNativeDisplayType native_display);
extern "C" EGLDisplay __wrap_eglGetDisplay(EGLNativeDisplayType native_display);

extern "C" EGLDisplay __wrap_eglGetDisplay(
    EGLNativeDisplayType native_display) {
  NativeDisplayType display_type;
  EssCtx *ctx = third_party::starboard::wpe::shared::Application::Get()->GetEssCtx();
  if (EssContextGetEGLDisplayType(ctx, &display_type))
    return __real_eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_type));
  return __real_eglGetDisplay(native_display);
}
