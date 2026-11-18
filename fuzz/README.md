# Fuzzers

libFuzzer harnesses for the parsers that eat hostile input (SPEC §11): the JSON
reader/writer, the form/query reader, the router, the cookie header and the
multipart reader.

```sh
make fuzz               # 10 s per harness, ASan + UBSan
make fuzz FUZZ_TIME=60  # longer
```

Each harness also asserts a *property*, not just absence of a crash: JSON
round-trips through the writer, captured router params stay inside the path,
and a cookie value stays inside its header. A parser that accepts everything is
as broken as one that crashes, and these harnesses are where that gets caught.

Corpora are not committed (they are inputs, not source); libFuzzer grows one per
harness under `build/fuzz/<name>_corpus/` and reuses it next run.
