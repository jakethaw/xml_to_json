# xml_to_json

Implementation of `xml_to_json(xml, indent)` function.

`xml_to_json(xml, indent)` takes two arguments:

* xml - XML string UTF-8 encoded
* indent - Indent for pretty printed JSON or -1 for minified JSON

The input XML is not validated prior to conversion.

# TOC

- [WebAssembly](#webassembly)
    - [Compile](#compile)
    - [Usage example:](#usage-example)
- [SQLite3](#sqlite3)
    - [Compile](#compile-1)
    - [Usage examples](#usage-examples)
- [Implementation Method](#implementation-method)
- [TODO](#todo)


# WebAssembly

## Demo

[jakethaw.com/xml_to_json](https://jakethaw.com/xml_to_json)

## Compile

To compile with [Emscripten](https://emscripten.org) as a WebAssembly function:

```bash
emcc -Oz xml_to_json.c -o xml_to_json.js -s EXPORTED_FUNCTIONS='["_xml_to_json", "_free"]' -s 'EXTRA_EXPORTED_RUNTIME_METHODS=["allocate", "intArrayFromString", "ALLOC_NORMAL", "UTF8ToString"]'
```

## Usage example: 

```javascript
var xml = allocate(intArrayFromString("<x>hello world</x>"), 'i8', ALLOC_NORMAL);
var indent = 2;

var json = _xml_to_json(xml, indent);
console.log(UTF8ToString(json, 5000));

_free(xml);
_free(json);
```
```json
{
  "x": "hello world"
}
```

# SQLite3

Implementation of an [SQLite3](sqlite.org) `xml_to_json(X, N)` function.

`xml_to_json(X, N)` takes one or two arguments:

* X - XML string UTF-8 encoded
* N - Optional indent for pretty printed JSON or -1 for minified JSON

The input XML is not validated prior to conversion.


## Compile

To compile with gcc as a run-time loadable extension:

```bash
UNIX-like : gcc -g -O3 -fPIC -shared xml_to_json.c -o xml_to_json.so -DSQLITE
Mac       : gcc -g -O3 -fPIC -dynamiclib xml_to_json.c -o xml_to_json.dylib -DSQLITE
Windows   : gcc -g -O3 -shared xml_to_json.c -o xml_to_json.dll -DSQLITE
```

Add the `-DDEBUG` option to print debug information to stdout.

E.g.

```bash
gcc -g -O3 -fPIC -shared xml_to_json.c -o xml_to_json.so -DDEBUG
```

## Usage examples

```sql
SELECT xml_to_json('<x>hello world</x>', 2);
```
```json
{
  "x": "hello world"
}
```
---
```sql
SELECT xml_to_json('<x>a<y/>b</x>', 2);
```
```json
{
  "x": {
    "#text": [
      "a",
      "b"
    ],
    "y": null
  }
}
```
---
```sql
SELECT xml_to_json('<x><y>abc</y><y>def</y></x>', 2);
```
```json
{
  "x": {
    "y": [
      "abc",
      "def"
    ]
  }
}
```
---
```sql
SELECT xml_to_json('<x>hello<y>abc</y>world<y>def</y>xyz</x>', 2);
```
```json
{
  "x": {
    "#text": [
      "hello",
      "world",
      "xyz"
    ],
    "y": [
      "abc",
      "def"
    ]
  }
}
```
---
```sql
SELECT xml_to_json('<x attr1="attr val 1" attr2="attr val 2">&amp; &gt; &lt; &#39;</x>', 2);
```
```json
{
  "x": {
    "@attr1": "attr val 1",
    "@attr2": "attr val 2",
    "#text": "& > < '"
  }
}
```

# Implementation Method

This implementation does not support the full [XML 1.0 Specification](https://www.w3.org/TR/REC-xml/). The following explaination is designed to describe what is currently supported.

TODO

# TODO

* Improve readme
* Add test cases
* Benchmark
