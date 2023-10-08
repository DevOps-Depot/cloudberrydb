#!/usr/bin/env bash

cat <<"EOF"
#!/usr/bin/env bash
if test -n "${ZSH_VERSION:-}"; then
    # zsh
    SCRIPT_PATH="${(%):-%x}"
elif test -n "${BASH_VERSION:-}"; then
    # bash
    SCRIPT_PATH="${BASH_SOURCE[0]}"
else
    # Unknown shell, hope below works.
    # Tested with dash
    result=$(lsof -p $$ -Fn | tail --lines=1 | xargs --max-args=2 | cut --delimiter=' ' --fields=2)
    SCRIPT_PATH=${result#n}
fi

if test -z "$SCRIPT_PATH"; then
    echo "The shell cannot be identified. \$GPHOME may not be set correctly." >&2
fi
SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" >/dev/null 2>&1 && pwd)"

if [ ! -L "${SCRIPT_DIR}" ]; then
    GPHOME=${SCRIPT_DIR}
else
    GPHOME=$(readlink "${SCRIPT_DIR}")
fi
EOF

cat <<EOF
PYTHONBINDIR="$(dirname "${WHICHPYTHON}")"
EOF

cat <<"EOF"
PYTHONPATH="${GPHOME}/lib/python"
PATH="${GPHOME}/bin:${PYTHONBINDIR}:${PATH}"
LD_LIBRARY_PATH="${GPHOME}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ -e "${GPHOME}/etc/openssl.cnf" ]; then
	OPENSSL_CONF="${GPHOME}/etc/openssl.cnf"
fi

#setup JAVA_HOME
if [ -x "${GPHOME}/ext/jdk/bin/java" ]; then
    JAVA_HOME="${GPHOME}/ext/jdk"
    PATH="${JAVA_HOME}/bin:${PATH}"
    CLASSPATH=${JAVA_HOME}/lib/dt.jar:${JAVA_HOME}/lib/tool.jar
fi

export GPHOME
export PATH
export PYTHONPATH
export LD_LIBRARY_PATH
export OPENSSL_CONF
export JAVA_HOME
export CLASSPATH
EOF
