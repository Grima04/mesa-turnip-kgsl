#!/usr/bin/env bash

TRACIE_SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
TRACES_YAML="$(readlink -f "$1")"
TRACE_TYPE="$2"

# Clone the traces-db repo without a checkout. Since we are dealing with
# git-lfs repositories, such clones are very lightweight. We check out
# individual files as needed at a later stage (see fetch_trace).
clone_traces_db_no_checkout()
{
    local repo="$1"
    local commit="$2"
    rm -rf traces-db
    git clone --no-checkout -c lfs.storage="$CI_PROJECT_DIR/.git-lfs-storage" "$repo" traces-db
    (cd traces-db; git reset "$commit" || git reset "origin/$commit")
}

query_traces_yaml()
{
    python3 "$TRACIE_SCRIPT_DIR/query_traces_yaml.py" \
        --file "$TRACES_YAML" "$@"
}

create_clean_git()
{
    rm -rf .clean_git
    cp -R .git .clean_git
}

restore_clean_git()
{
    rm -rf .git
    cp -R .clean_git .git
}

fetch_trace()
{
    local trace="${1//,/?}"
    echo -n "[fetch_trace] Fetching $1... "
    local output=$(git lfs pull -I "$trace" 2>&1)
    local ret=0
    if [[ $? -ne 0 || ! -f "$1" ]]; then
        echo "ERROR"
        echo "$output"
        ret=1
    else
        echo "OK"
    fi
    # Restore a clean .git directory, effectively removing any downloaded
    # git-lfs objects, in order to limit required storage. Note that the
    # checked out trace file is still present at this point. We remove it
    # when we are done with the trace replay at a later stage.
    restore_clean_git
    return $ret
}

get_dumped_file()
{
    local trace="$1"
    local tracedir="$(dirname "$trace")"
    local tracename="$(basename "$trace")"

    find "$tracedir/test/$DEVICE_NAME" -name "$tracename*.$2"
}

check_image()
{
    local trace="$1"
    local image="$2"

    checksum=$(python3 "$TRACIE_SCRIPT_DIR/image_checksum.py" "$image")
    expected=$(query_traces_yaml checksum --device-name "$DEVICE_NAME" "$trace")
    if [[ "$checksum" = "$expected" ]]; then
        echo "[check_image] Images match for $trace"
        return 0
    else
        echo "[check_image] Images differ for $trace (expected: $expected, actual: $checksum)"
        echo "[check_image] For more information see https://gitlab.freedesktop.org/mesa/mesa/blob/master/.gitlab-ci/tracie/README.md"
        return 1
    fi
}

archive_artifact()
{
    mkdir -p "$CI_PROJECT_DIR/results"
    cp --parents "$1" "$CI_PROJECT_DIR/results"
}

if [[ -n "$(query_traces_yaml traces_db_repo)" ]]; then
    clone_traces_db_no_checkout "$(query_traces_yaml traces_db_repo)" \
                                "$(query_traces_yaml traces_db_commit)"
    cd traces-db
else
    echo "Warning: No traces-db entry in $TRACES_YAML, assuming traces-db is current directory"
fi

# During git operations various git objects get created which
# may take up significant space. Store a clean .git instance,
# which we restore after various git operations to keep our
# storage consumption low.
create_clean_git

ret=0

for trace in $(query_traces_yaml traces --device-name "$DEVICE_NAME" --trace-types "$TRACE_TYPE")
do
    [[ -n "$(query_traces_yaml checksum --device-name "$DEVICE_NAME" "$trace")" ]] ||
        { echo "[fetch_trace] Skipping $trace since it has no checksums for $DEVICE_NAME"; continue; }
    fetch_trace "$trace" || exit $?
    python3 "$TRACIE_SCRIPT_DIR/dump_trace_images.py" --device-name "$DEVICE_NAME" "$trace" || exit $?
    image="$(get_dumped_file "$trace" png)"
    check_image "$trace" "$image" && check_succeeded=true || { ret=1; check_succeeded=false; }
    if [[ "$check_succeeded" = false || "$TRACIE_STORE_IMAGES" = "1" ]]; then
        archive_artifact "$image"
    fi
    archive_artifact "$(get_dumped_file "$trace" log)"
    # Remove the downloaded trace file to reduce the total amount of storage
    # that is required.
    rm "$trace"
done

exit $ret
