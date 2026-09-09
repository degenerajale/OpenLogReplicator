#!/usr/bin/env bash

set -u

OLR_BINARY="${1:-$(dirname "$0")/../cmake-build-debug/OpenLogReplicator}"
# Optional second argument: directory holding the Oracle instant client libraries,
# which the binary needs in the loader path
if [ -n "${2:-}" ] && [ -d "$2" ]; then
    export LD_LIBRARY_PATH="$2${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    [ -d "$2/libaio" ] && export LD_LIBRARY_PATH="$2/libaio:$LD_LIBRARY_PATH"
fi
if [ ! -x "$OLR_BINARY" ]; then
    echo "SKIP: OpenLogReplicator binary not found at: $OLR_BINARY"
    exit 77
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

FAILED=0

# run_test NAME CONFIG EXPECT_EXIT(0|nonzero|any) PATTERN...
# Every PATTERN must be found in the output.
run_test() {
    local name="$1" config="$2" expectExit="$3"
    shift 3
    local log exitCode pattern

    log="$WORKDIR/$name.log"
    # "any": the process may keep running with no broker/database to talk to, so use a short timeout
    if [ "$expectExit" = "any" ]; then local timeoutSecs=10; else local timeoutSecs=30; fi
    timeout $timeoutSecs "$OLR_BINARY" -f "$config" >"$log" 2>&1
    exitCode=$?

    if [ "$expectExit" = "nonzero" ]; then
        if [ $exitCode -eq 0 ]; then
            echo "FAIL $name: expected non-zero exit code, got 0"
            FAILED=1
            return
        fi
    elif [ "$expectExit" = "0" ] && [ $exitCode -ne 0 ]; then
        echo "FAIL $name: expected exit code 0, got: $exitCode"
        cat "$log"
        FAILED=1
        return
    fi
    # expectExit="any": the process may keep running (timeout) or die later on the
    # unreachable database - only the output patterns are asserted

    for pattern in "$@"; do
        if ! grep -q "$pattern" "$log"; then
            echo "FAIL $name: output does not contain: $pattern"
            cat "$log"
            FAILED=1
            return
        fi
    done
    echo "PASS $name"
}

cat >"$WORKDIR/source.json" <<'EOF'
{
  "version": "2.0.0",
  "memory": {"min-mb": 64, "max-mb": 1024},
  "source": [{
    "alias": "SOURCE",
    "name": "DBNAME",
    "reader": {"type": "online", "user": "user1", "password": "Password1", "server": "//localhost:1521/NOSERVICE"},
    "format": {"type": "json"},
    "filter": {"table": [{"owner": "HR", "table": "EMPLOYEES"}, {"owner": "HR", "table": "DEPTS"}]}
  }],
  "target": [{
    "alias": "KAFKA",
    "source": "SOURCE",
    "writer": {"type": "kafka", "topic": "olr_default", "topics": {__TOPICS__},
               "properties": {"bootstrap.servers": "localhost:9092"}}
  }]
}
EOF

make_config() {
    sed "s/__TOPICS__/$1/" "$WORKDIR/source.json" >"$WORKDIR/$2"
}

make_config '"HR.EMPLOYEES": "hr_employees", "hr.depts": "hr_employees"' topics-good.json
make_config '"NOTOPIC": "some_topic"' topics-bad-key.json
make_config '"HR.EMPLOYEES": 123' topics-bad-value.json
make_config '"HR.EMPLOYEES": "bad topic!"' topics-bad-charset.json
make_config '"HR.EMPLOYEES": "hr_employees", "HR.DEPTS": "other_topic", "HR.DEPTS": "hr_employees"' topics-dup-key.json
make_config '"": "some_topic"' topics-empty-key.json
make_config '"HR.EMPLOYEES": "."' topics-dot-name.json

# Valid mapping: parse succeeds and the mapping is logged, one line per entry (case folding included)
run_test topics-good "$WORKDIR/topics-good.json" any 'Kafka topic mapping: HR.EMPLOYEES -> hr_employees' 'HR.DEPTS -> hr_employees'

# Malformed mappings must be rejected at startup, naming the offending entry
run_test topics-bad-key "$WORKDIR/topics-bad-key.json" nonzero 'invalid "topics" key: "NOTOPIC"'
run_test topics-bad-value "$WORKDIR/topics-bad-value.json" nonzero 'invalid "topics" value for key "HR.EMPLOYEES"'
run_test topics-bad-charset "$WORKDIR/topics-bad-charset.json" nonzero 'invalid "topics" value for key "HR.EMPLOYEES": "bad topic!"'
run_test topics-dup-key "$WORKDIR/topics-dup-key.json" nonzero 'invalid "topics" key: "HR.DEPTS"'
run_test topics-empty-key "$WORKDIR/topics-empty-key.json" nonzero 'invalid "topics" key: ""'
run_test topics-dot-name "$WORKDIR/topics-dot-name.json" nonzero 'invalid "topics" value for key "HR.EMPLOYEES": "."'

# "topics" together with a "full" message format: one message can span several tables
cat >"$WORKDIR/topics-full.json" <<'EOF'
{
  "version": "2.0.0",
  "memory": {"min-mb": 64, "max-mb": 1024},
  "source": [{
    "alias": "SOURCE",
    "name": "DBNAME",
    "reader": {"type": "online", "user": "user1", "password": "Password1", "server": "//localhost:1521/NOSERVICE"},
    "format": {"type": "json", "message": 1},
    "filter": {"table": [{"owner": "HR", "table": "EMPLOYEES"}]}
  }],
  "target": [{
    "alias": "KAFKA",
    "source": "SOURCE",
    "writer": {"type": "kafka", "topic": "olr_default", "topics": {"HR.EMPLOYEES": "hr_employees"},
               "properties": {"bootstrap.servers": "localhost:9092"}}
  }]
}
EOF
run_test topics-full "$WORKDIR/topics-full.json" nonzero 'expected: not set when the message format is "full"'

# "topics" with a writer which is not kafka
cat >"$WORKDIR/topics-file.json" <<'EOF'
{
  "version": "2.0.0",
  "memory": {"min-mb": 64, "max-mb": 1024},
  "source": [{
    "alias": "SOURCE",
    "name": "DBNAME",
    "reader": {"type": "online", "user": "user1", "password": "Password1", "server": "//localhost:1521/NOSERVICE"},
    "format": {"type": "json"},
    "filter": {"table": [{"owner": "HR", "table": "EMPLOYEES"}]}
  }],
  "target": [{
    "alias": "FILE",
    "source": "SOURCE",
    "writer": {"type": "file", "output": "out.json", "topics": {"HR.EMPLOYEES": "hr_employees"}}
  }]
}
EOF
run_test topics-file "$WORKDIR/topics-file.json" nonzero 'expected: not set when "type" is not "kafka"'

# Without "topics" everything must still work as before
cat >"$WORKDIR/no-topics.json" <<'EOF'
{
  "version": "2.0.0",
  "memory": {"min-mb": 64, "max-mb": 1024},
  "source": [{
    "alias": "SOURCE",
    "name": "DBNAME",
    "reader": {"type": "online", "user": "user1", "password": "Password1", "server": "//localhost:1521/NOSERVICE"},
    "format": {"type": "json"},
    "filter": {"table": [{"owner": "HR", "table": "EMPLOYEES"}]}
  }],
  "target": [{
    "alias": "KAFKA",
    "source": "SOURCE",
    "writer": {"type": "kafka", "topic": "olr_default",
               "properties": {"bootstrap.servers": "localhost:9092"}}
  }]
}
EOF
run_test no-topics "$WORKDIR/no-topics.json" any 'adding target: KAFKA'

# Schemaless mode (flags: 2) is one of the paths that delivers a null table to the
# builder, so start the binary with the flag set and a mapping configured
sed 's/"alias": "SOURCE",/"alias": "SOURCE",\n    "flags": 2,/' "$WORKDIR/topics-good.json" >"$WORKDIR/topics-schemaless.json"
run_test topics-schemaless "$WORKDIR/topics-schemaless.json" any 'Kafka topic mapping: HR.EMPLOYEES -> hr_employees'

if [ $FAILED -ne 0 ]; then
    echo "config-parse test FAILED"
    exit 1
fi
echo "config-parse test passed"
exit 0
