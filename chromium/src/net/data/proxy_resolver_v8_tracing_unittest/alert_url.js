// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function FindProxyForURL(url, host) {
  alert(url);
  return "PROXY foobar:99";
}
