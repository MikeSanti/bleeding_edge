// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS d.file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library pub_tests;

import 'package:scheduled_test/scheduled_test.dart';

import '../descriptor.dart' as d;
import '../test_pub.dart';
import '../serve/utils.dart';

final transformer = """
import 'dart:async';

import 'package:barback/barback.dart';

class RewriteTransformer extends Transformer {
  RewriteTransformer.asPlugin();

  String get allowedExtensions => '.txt';

  Future apply(Transform transform) => throw new Exception('oh no!');
}
""";

main() {
  initConfig();
  integration("prints a transform error in apply", () {
    d.dir(appPath, [
      d.pubspec({
        "name": "myapp",
        "transformers": ["myapp/src/transformer"]
      }),
      d.dir("lib", [d.dir("src", [
        d.file("transformer.dart", transformer)
      ])]),
      d.dir("web", [
        d.file("foo.txt", "foo")
      ])
    ]).create();

    createLockFile('myapp', pkg: ['barback']);

    var server = pubServe();
    expect(server.nextErrLine(),
        completion(equals('Build error:')));
    expect(server.nextErrLine(), completion(equals('Transform Rewrite on '
        'myapp|web/foo.txt threw error: oh no!')));
    endPubServe();
  });
}
