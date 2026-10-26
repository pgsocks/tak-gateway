#!/bin/sh
set -eu

if [ "$#" -eq 0 ]; then
	set -- run
fi

TAK_GATEWAY_STATE_DIR=${TAK_GATEWAY_STATE_DIR:-/var/lib/tak-gateway}
export TAK_GATEWAY_STATE_DIR

# Keep original run arguments for exec; compose bootstrap in a subshell.
initialize() (
	shift
	selected_state=$TAK_GATEWAY_STATE_DIR
	state_selected=false
	while [ "$#" -gt 0 ]; do
		case "$1" in
		--state-dir)
			[ "$#" -ge 2 ] && [ -n "$2" ] || exit 0
			[ "$state_selected" = false ] || exit 0
			state_selected=true
			selected_state=$2
			shift 2
			;;
		*)
			# Let the executable handle help and invalid run arguments.
			exit 0
			;;
		esac
	done

	set -- config --if-uninitialized --state-dir "$selected_state"
	if [ -n "${TAK_GATEWAY_CONFIG_FILE:-}" ]; then
		set -- "$@" --config "$TAK_GATEWAY_CONFIG_FILE"
	fi
	if [ -n "${TAK_GATEWAY_IMPORT_ZIP:-}" ]; then
		set -- "$@" --import-zip "$TAK_GATEWAY_IMPORT_ZIP"
	fi
	while IFS= read -r item || [ -n "$item" ]; do
		if [ -n "$item" ]; then
			set -- "$@" --output "$item"
		fi
	done <<EOF
${TAK_GATEWAY_OUTPUTS:-}
EOF
	if [ -n "${TAK_GATEWAY_WEB_BIND:-}" ]; then
		set -- "$@" --web-bind "$TAK_GATEWAY_WEB_BIND"
	fi
	if [ -n "${TAK_GATEWAY_STATUS_FILE:-}" ]; then
		set -- "$@" --status-file "$TAK_GATEWAY_STATUS_FILE"
	fi
	if [ -n "${TAK_GATEWAY_LOG_LEVEL:-}" ]; then
		set -- "$@" --log-level "$TAK_GATEWAY_LOG_LEVEL"
	fi
	if [ -n "${TAK_GATEWAY_LOG_FORMAT:-}" ]; then
		set -- "$@" --log-format "$TAK_GATEWAY_LOG_FORMAT"
	fi
	while IFS= read -r item || [ -n "$item" ]; do
		if [ -n "$item" ]; then
			set -- "$@" --set "$item"
		fi
	done <<EOF
${TAK_GATEWAY_SET:-}
EOF

	# No initial values means no destructive empty-store initialization.
	if [ "$#" -gt 4 ]; then
		tak-gateway "$@"
	fi
)

if [ "$1" = run ]; then
	initialize "$@"
fi

exec tak-gateway "$@"
