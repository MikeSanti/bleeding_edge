// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library vmservice_test_helper;

import 'dart:async';
import 'dart:io';
import 'dart:json' as JSON;
import 'dart:utf' as UTF;
import 'package:expect/expect.dart';

abstract class VmServiceRequestHelper {
  final Uri uri;
  final HttpClient client;

  VmServiceRequestHelper(String url) :
      uri = Uri.parse(url),
      client = new HttpClient();

  Future makeRequest() {
    return client.getUrl(uri)
      .then((HttpClientRequest request) => request.close())
      .then((HttpClientResponse response) {
        return response
            .fold(new BytesBuilder(), (b, d) => b..add(d))
            .then((builder) {
              _requestCompleted(builder.takeBytes(), response);
            });
      }).catchError((error) {
        onRequestFailed(error);
      });
  }

  void _requestCompleted(List<int> data, HttpClientResponse response) {
    Expect.equals(200, response.statusCode, 'Invalid HTTP Status Code');
    var replyAsString;
    try {
      replyAsString = UTF.decodeUtf8(data, 0, null, null);
    } catch (e) {
      onRequestFailed(e);
      return;
    }
    var reply;
    try {
      reply = JSON.parse(replyAsString);
    } catch (e) {
      onRequestFailed(e);
      return;
    }
    if (reply is! Map) {
      onRequestFailed('Reply was not a map: $reply');
      return;
    }
    if (reply['type'] == null) {
      onRequestFailed('Reply does not contain a type key: $reply');
      return;
    }
    try {
      onRequestCompleted(reply);
    } catch (e) {
      onRequestFailed('Test callback failed: $e');
    }
  }

  void onRequestFailed(dynamic error) {
    Expect.fail('Failed to make request: $error');
  }

  void onRequestCompleted(Map response);
}

class TestLauncher {
  final String script;
  Process process;

  TestLauncher(this.script);

  String get scriptPath {
    var dartScript = Platform.script;
    var splitPoint = dartScript.lastIndexOf(Platform.pathSeparator);
    var scriptDirectory = dartScript.substring(0, splitPoint);
    return scriptDirectory + Platform.pathSeparator + script;
  }

  Future<int> launch() {
    String dartExecutable = Platform.executable;
    return Process.start(dartExecutable,
                         ['--enable-vm-service:0', scriptPath]).then((p) {
      Completer completer = new Completer();
      process = p;
      var portNumber;
      var blank;
      var first = true;
      process.stdout.transform(new StringDecoder())
                    .transform(new LineTransformer()).listen((line) {
        if (line.startsWith('VmService listening on port ')) {
          RegExp portExp = new RegExp(r"\d+");
          var port = portExp.stringMatch(line);
          portNumber = int.parse(port);
        }
        if (line == '') {
          // Received blank line.
          blank = true;
        }
        if (portNumber != null && blank == true && first == true) {
          completer.complete(portNumber);
          // Stop repeat completions.
          first = false;
        }
      });
      process.stderr.transform(new StringDecoder())
                    .transform(new LineTransformer()).listen((line) {
      });
      process.exitCode.then((_) { });
      return completer.future;
    });
  }

  void requestExit() {
    process.stdin.add([32]);
  }
}

class IsolateListTester {
  final Map isolateList;

  IsolateListTester(this.isolateList) {
    // The reply is an IsolateList.
    Expect.equals('IsolateList', isolateList['type'], 'Not an IsolateList.');
  }

  void checkIsolateCount(int n) {
    Expect.equals(n, isolateList['members'].length, 'Isolate count not $n');
  }

  void checkIsolateIdExists(int id) {
    var exists = false;
    isolateList['members'].forEach((isolate) {
      if (isolate['id'] == id) {
        exists = true;
      }
    });
    Expect.isTrue(exists, 'No isolate with id: $id');
  }

  void checkIsolateNameContains(String name) {
    var exists = false;
    isolateList['members'].forEach((isolate) {
      if (isolate['name'].contains(name)) {
        exists = true;
      }
    });
    Expect.isTrue(exists, 'No isolate with name: $name');
  }

  void checkIsolateNamePrefix(int id, String name) {
    var exists = false;
    isolateList['members'].forEach((isolate) {
      if (isolate['id'] == id) {
        exists = true;
        Expect.isTrue(isolate['name'].startsWith(name),
                      'Isolate $id does not have name prefix: $name');
      }
    });
    Expect.isTrue(exists, 'No isolate with id: $id');
  }
}