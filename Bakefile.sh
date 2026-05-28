# shellcheck shell=bash

init() {
	CFLAGS=(-Wall -Wextra -Wpedantic -fsanitize=address -g -Wno-unused-variable -Wno-unused-parameter)
}

task.build() {
	local dir=$1
	bake.assert_not_empty dir

	bear -- gcc "${CFLAGS[@]}" -DCONFIG_FILE="\"$dir\"" d.c -o ./d
}

task.install() {
	local prefix=${1:-/usr/local}
	sudo mkdir -p "$prefix/bin"
	sudo cp ./d "$prefix/bin/d"
}

task.run() {
	task.build
	DEBUG=1 ./main "$@"
}
