# A real interactive shell inside the visitor's browser guest.
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
export SHELL=/bin/bash
export LANG=C.UTF-8
export PS1='\[\e[38;5;148m\]silkie\[\e[0m\]:\w\$ '
umask 077

silkie-features
case "${SILKIE_DEMO_GAME:-}" in
    silkie-sprint) exec /usr/local/bin/silkie-sprint ;;
    silkie-scramble) exec /usr/local/bin/silkie-scramble ;;
    presidential) exec /usr/local/bin/presidential ;;
    silkie-loophole) exec /usr/local/bin/silkie-loophole ;;
esac
