# fm-shell.sh
#
# Wrapper de shell para o fm.
# Após sair do fm com 'q', o terminal muda automaticamente
# para o último diretório visitado.
#
# Adicione ao final do seu ~/.bashrc ou ~/.zshrc:
#   source /usr/local/bin/fm-shell.sh

fm() {
    local tmp
    tmp=$(mktemp /tmp/fm-lastdir.XXXXXX)

    /usr/local/bin/fm --lastdir "$tmp" "$@"

    local last
    last=$(cat "$tmp" 2>/dev/null)
    rm -f "$tmp"

    if [ -n "$last" ] && [ -d "$last" ] && [ "$last" != "$PWD" ]; then
        cd "$last" || true
        echo "  → $last"
    fi
}