#include "Methods.hpp"
#include <ncurses.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>
#include <set>

/*
 * ClipboardMode
 *
 * Indica o que está armazenado no clipboard interno no momento:
 *   CLIP_EMPTY    → nada copiado ainda
 *   CLIP_CONTENT  → conteúdo de um arquivo (yc)
 *
 * Nota: yy copia o path direto para o clipboard do sistema e não
 * utiliza o clipboard interno — por isso CLIP_PATH foi removido.
 */
enum ClipboardMode
{
    CLIP_EMPTY,
    CLIP_CONTENT
};

/*
 * Clipboard
 *
 * Estrutura que representa o estado global do clipboard interno.
 * Usado exclusivamente pelo par yc/pp para copiar e colar conteúdo
 * de arquivo dentro do fm.
 *
 *   mode     indica se há conteúdo disponível para colar
 *   content  conteúdo bruto do arquivo copiado via yc
 *   src      caminho original do arquivo copiado (exibido no rodapé)
 */
struct Clipboard
{
    ClipboardMode  mode    = CLIP_EMPTY;
    std::string    content = "";
    fs::path       src     = "";
};

/*
 * UndoEntry
 *
 * Representa uma entrada que foi deletada e pode ser restaurada.
 * O arquivo é movido para um diretório temporário (~/.fm_trash/)
 * em vez de ser permanentemente removido.
 *
 *   trash_path   caminho onde o arquivo foi guardado no trash
 *   origin_path  caminho original de onde o arquivo veio
 */
struct UndoEntry
{
    fs::path trash_path;
    fs::path origin_path;
};

/*
 * UndoStack
 *
 * Pilha de operações de deleção que podem ser desfeitas com 'u'.
 * Cada deleção empilha um UndoEntry. O undo desempilha o topo
 * e move o arquivo de volta ao caminho original.
 *
 * O diretório de trash é criado na primeira deleção.
 */
struct UndoStack
{
    std::vector < UndoEntry > stack;

    fs::path trash_dir ( )
    {
        const char * home = getenv ( "HOME" );
        fs::path dir = home ? fs::path ( home ) / ".fm_trash" : fs::path ( "/tmp/.fm_trash" );

        std::error_code ec;
        fs::create_directories ( dir, ec );
        return dir;
    }

    /*
     * push_delete
     *
     * Move entry de origin para o trash e empilha o UndoEntry.
     * Retorna true se a operação foi concluída com sucesso.
     *
     * Parâmetros:
     *   origin  caminho completo da entrada a deletar
     */
    bool push_delete ( const fs::path & origin )
    {
        fs::path tdir = trash_dir ( );

        /* nome único no trash para evitar colisões */
        fs::path dst = tdir / origin.filename ( );
        int suffix = 0;
        while ( fs::exists ( dst ) )
        {
            dst = tdir / ( origin.filename ( ).string ( )
                           + "." + std::to_string ( ++ suffix ) );
        }

        std::error_code ec;
        fs::rename ( origin, dst, ec );

        if ( ec )
        {
            /* rename entre filesystems falha — tenta copy + remove */
            fs::copy_options opts =
                fs::copy_options::recursive |
                fs::copy_options::copy_symlinks;

            fs::copy ( origin, dst, opts, ec );
            if ( ec ) return false;

            fs::remove_all ( origin, ec );
            if ( ec ) return false;
        }

        stack.push_back ( { dst, origin } );
        return true;
    }

    /*
     * pop_undo
     *
     * Restaura a última entrada deletada ao seu caminho original.
     * Retorna true se restaurado com sucesso, false se a pilha
     * estiver vazia ou o destino já existir.
     */
    bool pop_undo ( )
    {
        if ( stack.empty ( ) ) return false;

        UndoEntry top = stack.back ( );
        stack.pop_back ( );

        if ( fs::exists ( top.origin_path ) ) return false;

        std::error_code ec;
        fs::rename ( top.trash_path, top.origin_path, ec );

        if ( ec )
        {
            fs::copy_options opts =
                fs::copy_options::recursive |
                fs::copy_options::copy_symlinks;

            fs::copy ( top.trash_path, top.origin_path, opts, ec );
            if ( ec ) return false;

            fs::remove_all ( top.trash_path, ec );
        }

        return ! ec;
    }

    bool empty ( ) const { return stack.empty ( ); }
};

/*
 * Selection
 *
 * Conjunto de entradas marcadas para operações em lote (mover).
 * O usuário seleciona com Space e executa com 'M' (Shift+M).
 * Entradas selecionadas são exibidas com prefixo [*D] ou [*F].
 * ESC limpa a seleção sem mover nada.
 */
struct Selection
{
    std::set < std::string > names;

    void toggle ( const std::string & name )
    {
        if ( names.count ( name ) )
            names.erase ( name );
        else
            names.insert ( name );
    }

    bool has ( const std::string & name ) const
    {
        return names.count ( name ) > 0;
    }

    void clear ( ) { names.clear ( ); }
    bool empty ( ) const { return names.empty ( ); }
    int  size  ( ) const { return ( int ) names.size ( ); }
};

/*
 * KeySeq
 *
 * Gerencia sequências de duas teclas no estilo vim (ex: dd, yy, yc, pp).
 * Armazena a primeira tecla pressionada e o timestamp, descartando
 * a sequência se a segunda tecla demorar mais de TIMEOUT_MS milissegundos.
 *
 * Mapa de sequências reconhecidas:
 *   yc  → SEQ_YC  copia conteúdo do arquivo para clipboard do sistema e interno
 *   yy  → SEQ_YY  copia path da entrada para o clipboard do sistema
 *   pp  → SEQ_PP  cola conteúdo copiado via yc como novo arquivo
 *   dd  → SEQ_DD  move a entrada selecionada para o trash
 *
 * Teclas que NÃO participam de sequência (chegam direto ao loop):
 *   a   cria novo arquivo
 *   A   cria nova pasta
 *   r   renomeia entrada
 *   u   desfaz última deleção
 *   /   busca por prefixo
 *   s   cicla modo de ordenação
 *   P   copia path atual para o clipboard do sistema
 *   C   clona entrada selecionada
 *   M   move seleção para destino
 *   q   encerra o programa
 */
struct KeySeq
{
    static constexpr int SEQ_YC = -1;   /* yc → copiar conteúdo do arquivo */
    static constexpr int SEQ_DD = -2;   /* dd → mover para trash            */
    static constexpr int SEQ_YY = -3;   /* yy → copiar caminho da entrada   */
    static constexpr int SEQ_PP = -4;   /* pp → colar                       */

    static constexpr int TIMEOUT_MS = 400;

    using Clock = std::chrono::steady_clock;

    int  first     = 0;
    bool waiting   = false;
    Clock::time_point last_time;

    /*
     * push
     *
     * Recebe uma tecla e retorna:
     *   0          → aguardando o segundo caractere da sequência
     *   SEQ_*      → sequência de dois caracteres completa
     *   key        → tecla avulsa (não inicia sequência ou timeout expirou)
     *
     * Apenas 'y', 'p' e 'd' iniciam sequências. As teclas 'a', 'A', 'r',
     * 'u', '/', 's', 'P', 'C', 'M' e 'q' passam direto sem espera.
     */
    int push ( int key )
    {
        auto now = Clock::now ( );

        if ( waiting )
        {
            auto elapsed = std::chrono::duration_cast < std::chrono::milliseconds >
                ( now - last_time ).count ( );

            bool timed_out = ( elapsed > TIMEOUT_MS );
            bool same_key  = ( key == first );

            waiting = false;

            if ( ! timed_out && same_key )
            {
                switch ( first )
                {
                    case 'd' : return SEQ_DD;
                    case 'p' : return SEQ_PP;
                    default  : return key;
                }
            }

            /* sequência incompleta ou timeout: verifica combinações heterogêneas */
            if ( ! timed_out && first == 'y' )
            {
                if ( key == 'y' ) return SEQ_YY;
                if ( key == 'c' ) return SEQ_YC;
            }

            return key;
        }

        /* teclas que iniciam sequência */
        if ( key == 'y' || key == 'p' || key == 'd' )
        {
            first     = key;
            waiting   = true;
            last_time = now;
            return 0;
        }

        return key;
    }
};

/*
 * ncurses_init
 */
static void ncurses_init ( )
{
    initscr ( );
    noecho ( );
    cbreak ( );
    keypad ( stdscr, TRUE );
    curs_set ( 0 );
    timeout ( 100 );
}

/*
 * prompt_input
 *
 * Exibe uma linha de input no rodapé da tela e captura uma string.
 * Suporta backspace e ESC para cancelar.
 */
static std::string prompt_input ( const std::string & label )
{
    int rows, cols;
    getmaxyx ( stdscr, rows, cols );

    const int row = rows - 1;

    echo ( );
    curs_set ( 1 );
    timeout ( -1 );

    std::string input;
    int ch;

    while ( true )
    {
        move ( row, 0 );
        clrtoeol ( );
        mvprintw ( row, 0, "%s%s", label.c_str ( ), input.c_str ( ) );
        refresh ( );

        ch = getch ( );

        if ( ch == 27 )
        {
            input.clear ( );
            break;
        }

        if ( ch == '\n' || ch == KEY_ENTER )
            break;

        if ( ( ch == KEY_BACKSPACE || ch == 127 || ch == 8 ) && ! input.empty ( ) )
            input.pop_back ( );
        else if ( ch >= 32 && ch < 127 )
            input += static_cast < char > ( ch );
    }

    noecho ( );
    curs_set ( 0 );
    timeout ( 100 );

    return input;
}

/*
 * open_editor
 *
 * Suspende o ncurses e abre o arquivo no editor ($EDITOR ou vim).
 */
static void open_editor ( const fs::path & file )
{
    const char * editor = getenv ( "EDITOR" );
    if ( ! editor )
        editor = "vim";

    std::string path = file.string ( );

    def_prog_mode ( );
    endwin ( );

    pid_t pid = fork ( );

    if ( pid == 0 )
    {
        char * args [ ] =
        {
            ( char * ) editor,
            ( char * ) path.c_str ( ),
            NULL
        };
        execvp ( editor, args );
        exit ( 1 );
    }
    if ( pid > 0 )
        waitpid ( pid, NULL, 0 );

    reset_prog_mode ( );
    refresh ( );
}

/*
 * create_file
 *
 * Solicita um nome ao usuário e cria um arquivo vazio no diretório atual.
 * Ativado pela tecla 'a' (add file).
 * Retorna false se o nome estiver vazio ou o arquivo já existir.
 */
static bool create_file ( const fs::path & path )
{
    std::string name = prompt_input ( "Novo arquivo: " );
    if ( name.empty ( ) ) return false;

    fs::path target = path / name;
    if ( fs::exists ( target ) ) return false;

    std::ofstream f ( target );
    return f.good ( );
}

/*
 * create_dir
 *
 * Solicita um nome ao usuário e cria um diretório no diretório atual.
 * Ativado pela tecla 'A' (Add directory).
 * Retorna false se o nome estiver vazio ou o diretório já existir.
 */
static bool create_dir ( const fs::path & path )
{
    std::string name = prompt_input ( "Nova pasta: " );
    if ( name.empty ( ) ) return false;

    fs::path target = path / name;
    if ( fs::exists ( target ) ) return false;

    return fs::create_directory ( target );
}

/*
 * rename_entry
 */
static bool rename_entry ( const fs::path & path, const Entry * entry )
{
    std::string new_name = prompt_input ( "Renomear para: " );
    if ( new_name.empty ( ) ) return false;

    fs::path src = path / entry -> getName ( );
    fs::path dst = path / new_name;

    if ( fs::exists ( dst ) ) return false;

    fs::rename ( src, dst );
    return true;
}

/*
 * copy_to_system_clipboard
 *
 * Envia uma string para o clipboard do sistema.
 * Tenta wl-copy (Wayland) primeiro; cai para xclip (X11) se falhar.
 *
 * Parâmetros:
 *   value  string a ser copiada
 */
static void copy_to_system_clipboard ( const std::string & value )
{
    std::string cmd =
        "printf '%s' '" + value + "' | wl-copy 2>/dev/null || "
        "printf '%s' '" + value + "' | xclip -selection clipboard 2>/dev/null";
    system ( cmd.c_str ( ) );
}

/*
 * cmd_yc
 *
 * Copia o conteúdo do arquivo selecionado para o clipboard do sistema
 * e para o clipboard interno (usado pelo pp para colar dentro do fm).
 * Ativado pela sequência 'yc' (yank content).
 * Só funciona para entradas do tipo ENTRY_FILE.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 *   clip   clipboard interno a ser preenchido
 *
 * Retorno:
 *   true se o conteúdo foi lido com sucesso
 */
static bool cmd_yc ( const fs::path & path, const Entry * entry, Clipboard & clip )
{
    if ( entry -> getType ( ) != ENTRY_FILE )
        return false;

    fs::path src = path / entry -> getName ( );
    std::ifstream f ( src );
    if ( ! f.is_open ( ) ) return false;

    std::ostringstream ss;
    ss << f.rdbuf ( );

    clip.content = ss.str ( );
    clip.src     = src;
    clip.mode    = CLIP_CONTENT;

    /* espelha no clipboard do sistema para uso fora do fm */
    copy_to_system_clipboard ( clip.content );

    return true;
}

/*
 * cmd_yy
 *
 * Copia o path completo da entrada selecionada para o clipboard do sistema.
 * Ativado pela sequência 'yy' (yank path).
 * Funciona para arquivos e pastas.
 * Não altera o clipboard interno — yy e pp são independentes.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 */
static void cmd_yy ( const fs::path & path, const Entry * entry )
{
    copy_to_system_clipboard ( ( path / entry -> getName ( ) ).string ( ) );
}

/*
 * cmd_pp
 *
 * Cola o conteúdo copiado via yc como um novo arquivo no diretório atual.
 * Ativado pela sequência 'pp' (paste).
 *
 * Solicita o nome do arquivo de destino via prompt. Cancela se o nome
 * estiver vazio ou o destino já existir.
 *
 * Parâmetros:
 *   path  diretório de destino
 *   clip  clipboard interno (deve estar no modo CLIP_CONTENT)
 *
 * Retorno:
 *   true se o arquivo foi criado com sucesso
 */
static bool cmd_pp ( const fs::path & path, const Clipboard & clip )
{
    if ( clip.mode == CLIP_EMPTY )
        return false;

    std::string name = prompt_input ( "Colar como: " );
    if ( name.empty ( ) ) return false;

    fs::path dst = path / name;
    if ( fs::exists ( dst ) ) return false;

    std::ofstream f ( dst );
    if ( ! f.is_open ( ) ) return false;
    f << clip.content;
    return f.good ( );
}

/*
 * cmd_dd
 *
 * Move a entrada para o trash (via UndoStack) após confirmação.
 * Ativado pela sequência 'dd' (delete).
 * O arquivo pode ser restaurado com 'u'.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 *   undo   pilha de undo onde a operação será empilhada
 *
 * Retorno:
 *   true  se movido para o trash com sucesso
 *   false se cancelado ou operação falhar
 */
static bool cmd_dd ( const fs::path & path, const Entry * entry, UndoStack & undo )
{
    std::string confirm = prompt_input (
        "Deletar \"" + entry -> getName ( ) + "\"? (s/n): "
    );

    if ( confirm != "s" && confirm != "S" )
        return false;

    fs::path target = path / entry -> getName ( );
    return undo.push_delete ( target );
}

/*
 * copy_current_path
 *
 * Copia o caminho do diretório atual para o clipboard do sistema.
 * Ativado pela tecla 'P'.
 */
static void copy_current_path ( const fs::path & path )
{
    copy_to_system_clipboard ( path.string ( ) );
}

/*
 * clone_entry
 *
 * Cria uma cópia da entrada selecionada no mesmo diretório.
 * Ativado pela tecla 'C'. O sufixo "_copy" é adicionado ao nome;
 * se já existir, incrementa com "_copy_N".
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada a clonar
 *
 * Retorno:
 *   true se a cópia foi criada com sucesso
 */
static bool clone_entry ( const fs::path & path, const Entry * entry )
{
    fs::path src = path / entry -> getName ( );
    fs::path dst = path / ( entry -> getName ( ) + "_copy" );
    int i = 1;
    while ( fs::exists ( dst ) )
        dst = path / ( entry -> getName ( ) + "_copy_" + std::to_string ( i ++ ) );
    std::error_code ec;
    fs::copy ( src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec );
    return ! ec;
}

/*
 * cmd_move_selection
 *
 * Move todos os arquivos/pastas selecionados para um diretório de destino.
 * Ativado por 'M' (Shift+M). Solicita o caminho destino via prompt.
 * Suporta expansão de '~' para o diretório home do usuário.
 * Cria o diretório destino se não existir.
 * Não sobrescreve entradas já existentes no destino.
 * Limpa a seleção após mover com sucesso.
 *
 * Parâmetros:
 *   path  diretório atual (origem)
 *   sel   conjunto de entradas selecionadas
 *
 * Retorno:
 *   true se pelo menos uma entrada foi movida com sucesso
 */
static bool cmd_move_selection ( const fs::path & path, Selection & sel )
{
    if ( sel.empty ( ) ) return false;

    std::string dest_str = prompt_input ( "Mover para: " );
    if ( dest_str.empty ( ) ) return false;

    fs::path dest = dest_str;

    /* expande ~ para o home do usuário */
    if ( ! dest_str.empty ( ) && dest_str [ 0 ] == '~' )
    {
        const char * home = getenv ( "HOME" );
        if ( home )
            dest = fs::path ( home ) / dest_str.substr ( 1 );
    }

    std::error_code ec;

    /* cria o diretório destino se não existir */
    if ( ! fs::exists ( dest ) )
        fs::create_directories ( dest, ec );

    if ( ! fs::is_directory ( dest ) ) return false;

    bool any_moved = false;

    for ( const auto & name : sel.names )
    {
        fs::path src = path / name;
        fs::path dst = dest / name;

        if ( ! fs::exists ( src ) ) continue;

        /* não sobrescreve entrada já existente no destino */
        if ( fs::exists ( dst ) ) continue;

        fs::rename ( src, dst, ec );

        if ( ec )
        {
            /* fallback para cross-filesystem: copy + remove */
            fs::copy ( src, dst,
                       fs::copy_options::recursive |
                       fs::copy_options::copy_symlinks, ec );
            if ( ! ec )
            {
                fs::remove_all ( src, ec );
                any_moved = true;
            }
        }
        else
        {
            any_moved = true;
        }
    }

    if ( any_moved )
        sel.clear ( );

    return any_moved;
}

/*
 * sort_label
 *
 * Retorna uma string curta descrevendo o modo de ordenação atual.
 */
static const char * sort_label ( SortMode mode )
{
    switch ( mode )
    {
        case SORT_NAME : return "nome";
        case SORT_SIZE : return "tamanho";
        case SORT_DATE : return "data";
    }
    return "";
}

/*
 * clip_status
 *
 * Retorna uma string de status do clipboard interno para exibição no rodapé.
 * Exibe [yc] e o nome do arquivo quando há conteúdo disponível para pp.
 * Vazio se não houver nada copiado.
 */
static std::string clip_status ( const Clipboard & clip )
{
    if ( clip.mode == CLIP_EMPTY ) return "";
    return "[yc] " + clip.src.filename ( ).string ( );
}

/*
 * render
 *
 * Redesenha toda a interface usando flatten() — O(n) total,
 * eliminando o custo O(n²) do loop original com get(i).
 *
 * Entradas com match de busca ativa são destacadas com A_BOLD.
 * A entrada selecionada usa A_REVERSE.
 * Entradas marcadas para mover exibem prefixo [*D] ou [*F].
 *
 * Parâmetros:
 *   m            referência ao objeto Methods
 *   cursor       índice da entrada selecionada
 *   path         caminho atual
 *   clip         clipboard atual
 *   undo         pilha de undo (para indicar se há undo disponível)
 *   sel          seleção atual para mover
 *   search_name  nome da entrada buscada (vazio = sem busca ativa)
 */
static void render
(
    const Methods     & m,
    int                 cursor,
    const fs::path    & path,
    const Clipboard   & clip,
    const UndoStack   & undo,
    const Selection   & sel,
    const std::string & search_name = ""
)
{
    clear ( );

    int rows, cols;
    getmaxyx ( stdscr, rows, cols );

    const int list_rows = rows - 5; /* linhas disponíveis para a listagem */

    /* — flatten: O(n) em vez de O(n²) — */
    Entry * flat [ FM_MAX_ENTRIES ];
    int n = m.flatten ( ( Entry ** ) flat );

    if ( n == 0 )
    {
        mvprintw ( 0, 0, "(diretório vazio)" );
    }
    else
    {
        /* scroll: garante que o cursor fique sempre visível */
        int scroll = 0;
        if ( cursor >= list_rows )
            scroll = cursor - list_rows + 1;

        int display_end = scroll + list_rows;
        if ( display_end > n ) display_end = n;

        for ( int i = scroll; i < display_end; i ++ )
        {
            int row = i - scroll;

            bool is_cursor   = ( i == cursor );
            bool is_match    = ( ! search_name.empty ( )
                                 && flat [ i ] -> getName ( ) == search_name );
            bool is_selected = sel.has ( flat [ i ] -> getName ( ) );

            if ( is_cursor   ) attron  ( A_REVERSE );
            if ( is_match    ) attron  ( A_BOLD    );
            if ( is_selected ) attron  ( A_BOLD    );

            /* prefixo: [*D]/[*F] se selecionado, [D]/[F] caso contrário */
            const char * prefix;
            if ( is_selected )
                prefix = ( flat [ i ] -> getType ( ) == ENTRY_DIR ) ? "[*D]" : "[*F]";
            else
                prefix = ( flat [ i ] -> getType ( ) == ENTRY_DIR ) ? "[D] " : "[F] ";

            mvprintw ( row, 0, "%s%s", prefix, flat [ i ] -> getName ( ).c_str ( ) );

            if ( is_selected ) attroff ( A_BOLD    );
            if ( is_match    ) attroff ( A_BOLD    );
            if ( is_cursor   ) attroff ( A_REVERSE );
        }
    }

    /* — rodapé — */
    mvprintw ( rows - 5, 0, "%-*s", cols, path.c_str ( ) );

    /* status: clipboard + seleção + undo + ordenação */
    std::string status;
    std::string cs = clip_status ( clip );
    if ( ! cs.empty ( )   ) status += cs + "  ";
    if ( ! sel.empty ( )  ) status += "[sel: " + std::to_string ( sel.size ( ) ) + "]  ";
    if ( ! undo.empty ( ) ) status += "[u: desfazer]  ";
    status += std::string ( "ord: " ) + sort_label ( m.getSortMode ( ) );

    mvprintw ( rows - 4, 0, "%-*s", cols, status.c_str ( ) );

    /* busca ativa */
    if ( ! search_name.empty ( ) )
        mvprintw ( rows - 3, 0, "busca: %s", search_name.c_str ( ) );
    else
        mvprintw ( rows - 3, 0, "%-*s", cols, "" );

    /* mapa de teclas */
    mvprintw
    (
        rows - 2,
        0,
        "Enter-abrir  Bksp-voltar  yc-copiar conteudo  yy-copiar entrada  "
        "pp-colar  dd-deletar  u-desfazer  a-criar arq  A-criar pasta  "
        "r-renomear  /-buscar  s-ordenar  P-copiar path  C-clonar  "
        "Spc-selecionar  M-mover selecao  ESC-limpar sel  q-sair"
    );

    refresh ( );
}

/*
 * cmd_search
 *
 * Solicita um prefixo ao usuário e usa search_prefix da BST
 * para localizar entradas. Retorna o índice in-order do primeiro
 * resultado, ou -1 se não encontrado.
 *
 * A busca tenta primeiro ENTRY_DIR, depois ENTRY_FILE.
 * O nome do primeiro match é devolvido em result_name para
 * que o render possa destacá-lo.
 *
 * Complexidade da busca: O(log n + k)
 *
 * Parâmetros:
 *   m            referência ao Methods
 *   result_name  saída: nome da primeira entrada encontrada
 *
 * Retorno:
 *   índice in-order do primeiro resultado, ou -1 se não encontrado
 */
static int cmd_search ( const Methods & m, std::string & result_name )
{
    std::string prefix = prompt_input ( "/buscar: " );
    result_name.clear ( );

    if ( prefix.empty ( ) )
        return -1;

    Entry * results [ FM_MAX_ENTRIES ];
    int found = m.search_prefix ( prefix, ( Entry ** ) results, FM_MAX_ENTRIES );

    if ( found == 0 )
        return -1;

    result_name = results [ 0 ] -> getName ( );
    return m.find_index ( result_name );
}

/*
 * main
 *
 * Loop principal do navegador de arquivos.
 *
 * Mapa completo de teclas e sequências:
 *
 *   Navegação:
 *     KEY_DOWN         desce o cursor (circular)
 *     KEY_UP           sobe o cursor (circular)
 *     Enter            entra em pasta / abre arquivo no editor
 *     Backspace        sobe um nível no diretório
 *
 *   Clipboard:
 *     yc               copia conteúdo do arquivo → clipboard do sistema + interno (pp)
 *     yy               copia path da entrada → clipboard do sistema apenas
 *     pp               cola conteúdo de yc como novo arquivo no dir atual
 *     dd               move para trash (desfazível com u)
 *
 *   Seleção e mover:
 *     Space            seleciona/deseleciona entrada sob o cursor e avança cursor
 *     M                move todas as entradas selecionadas para destino informado
 *     ESC              limpa a seleção sem mover
 *
 *   Edição (teclas avulsas, chegam direto ao loop):
 *     a                cria novo arquivo (add file)
 *     A                cria nova pasta (Add directory)
 *     r                renomeia entrada selecionada
 *     u                desfaz última deleção (restaura do trash)
 *
 *   Utilitários:
 *     /                busca por prefixo na BST — O(log n + k)
 *     s                cicla modo de ordenação (nome → tamanho → data)
 *     P                copia path do diretório atual para clipboard do sistema
 *     C                clona arquivo/pasta selecionado no mesmo diretório
 *     q                encerra o programa
 */
int main ( int argc, char ** argv )
{
    fs::path   path = fs::current_path ( );
    Methods    m ( FM_MAX_ENTRIES );
    Clipboard  clip;
    KeySeq     seq;
    UndoStack  undo;
    Selection  sel;

    std::string search_name; /* nome do último resultado de busca */

    m.load ( path );

    ncurses_init ( );

    int cursor = 0;
    int key;

    render ( m, cursor, path, clip, undo, sel, search_name );

    while ( true )
    {
        key = getch ( );

        if ( key == ERR )
        {
            render ( m, cursor, path, clip, undo, sel, search_name );
            continue;
        }

        int cmd = seq.push ( key );

        if ( cmd == 0 )
            continue;

        if ( cmd == 'q' )
            break;

        if ( cmd == 'P' )
            copy_current_path ( path );

        const int count = m.count ( );

        if ( cmd == 'C' && count > 0 )
        {
            clone_entry ( path, m.get ( cursor ) );
            m.load ( path );
        }

        /* — navegação — */
        if ( cmd == KEY_DOWN )
        {
            cursor = ( count > 0 ) ? ( cursor + 1 ) % count : 0;
            search_name.clear ( ); /* limpa destaque ao mover */
        }

        if ( cmd == KEY_UP )
        {
            cursor = ( count > 0 ) ? ( cursor - 1 + count ) % count : 0;
            search_name.clear ( );
        }

        /* — entrar em pasta / abrir arquivo — */
        if ( ( cmd == KEY_ENTER || cmd == '\n' ) && count > 0 )
        {
            auto entry = m.get ( cursor );

            if ( entry -> getType ( ) == ENTRY_DIR )
            {
                fs::path next = path / entry -> getName ( );
                if ( fs::exists ( next ) && fs::is_directory ( next ) )
                {
                    path = fs::canonical ( next );
                    m.load ( path );
                    cursor = 0;
                    search_name.clear ( );
                    sel.clear ( ); /* limpa seleção ao trocar de diretório */
                }
            }
            else if ( entry -> getType ( ) == ENTRY_FILE )
            {
                open_editor ( path / entry -> getName ( ) );
                m.load ( path );
            }
        }

        /* — voltar um nível — */
        if ( cmd == KEY_BACKSPACE || cmd == 127 || cmd == 8 )
        {
            path = path.parent_path ( );
            m.load ( path );
            cursor = 0;
            search_name.clear ( );
            sel.clear ( ); /* limpa seleção ao trocar de diretório */
        }

        /* — criar arquivo — */
        if ( cmd == 'a' )
        {
            create_file ( path );
            m.load ( path );
        }

        /* — criar pasta — */
        if ( cmd == 'A' )
        {
            create_dir ( path );
            m.load ( path );
        }

        /* — renomear — */
        if ( cmd == 'r' && count > 0 )
        {
            rename_entry ( path, m.get ( cursor ) );
            m.load ( path );
            search_name.clear ( );
        }

        /* — yc: copiar conteúdo do arquivo — */
        if ( cmd == KeySeq::SEQ_YC && count > 0 )
            cmd_yc ( path, m.get ( cursor ), clip );

        /* — yy: copiar path da entrada para clipboard do sistema — */
        if ( cmd == KeySeq::SEQ_YY && count > 0 )
            cmd_yy ( path, m.get ( cursor ) );

        /* — pp: colar — */
        if ( cmd == KeySeq::SEQ_PP )
        {
            cmd_pp ( path, clip );
            m.load ( path );
        }

        /* — dd: mover para trash — */
        if ( cmd == KeySeq::SEQ_DD && count > 0 )
        {
            if ( cmd_dd ( path, m.get ( cursor ), undo ) )
            {
                m.load ( path );
                if ( cursor >= m.count ( ) && cursor > 0 )
                    cursor --;
                search_name.clear ( );
            }
        }

        /* — u: desfazer última deleção — */
        if ( cmd == 'u' )
        {
            if ( undo.pop_undo ( ) )
            {
                m.load ( path );
                search_name.clear ( );
            }
        }

        /* — /: busca por prefixo na BST — */
        if ( cmd == '/' )
        {
            int idx = cmd_search ( m, search_name );
            if ( idx >= 0 )
                cursor = idx;
            /* se não encontrado, search_name fica vazio e render mostra nada */
        }

        /* — s: ciclar modo de ordenação — */
        if ( cmd == 's' )
        {
            SortMode next;
            switch ( m.getSortMode ( ) )
            {
                case SORT_NAME : next = SORT_SIZE; break;
                case SORT_SIZE : next = SORT_DATE; break;
                case SORT_DATE : next = SORT_NAME; break;
                default        : next = SORT_NAME;
            }
            m.setSortMode ( next, path );
            cursor = 0;
            search_name.clear ( );
        }

        /* — Space: selecionar/deselecionar entrada e avançar cursor — */
        if ( cmd == ' ' && count > 0 )
        {
            sel.toggle ( m.get ( cursor ) -> getName ( ) );
            /* avança cursor automaticamente para agilizar seleção em sequência */
            cursor = ( cursor + 1 ) % count;
        }

        /* — M: mover seleção para destino — */
        if ( cmd == 'M' && ! sel.empty ( ) )
        {
            if ( cmd_move_selection ( path, sel ) )
            {
                m.load ( path );
                if ( cursor >= m.count ( ) && cursor > 0 )
                    cursor --;
                search_name.clear ( );
            }
        }

        /* — ESC: limpar seleção — */#include "Methods.hpp"
#include <ncurses.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>
#include <set>

/*
 * ClipboardMode
 *
 * Indica o que está armazenado no clipboard interno no momento:
 *   CLIP_EMPTY    → nada copiado ainda
 *   CLIP_CONTENT  → conteúdo de um arquivo (yc)
 *
 * Nota: yy copia o path direto para o clipboard do sistema e não
 * utiliza o clipboard interno — por isso CLIP_PATH foi removido.
 */
enum ClipboardMode
{
    CLIP_EMPTY,
    CLIP_CONTENT
};

/*
 * Clipboard
 *
 * Estrutura que representa o estado global do clipboard interno.
 * Usado exclusivamente pelo par yc/pp para copiar e colar conteúdo
 * de arquivo dentro do fm.
 *
 *   mode     indica se há conteúdo disponível para colar
 *   content  conteúdo bruto do arquivo copiado via yc
 *   src      caminho original do arquivo copiado (exibido no rodapé)
 */
struct Clipboard
{
    ClipboardMode  mode    = CLIP_EMPTY;
    std::string    content = "";
    fs::path       src     = "";
};

/*
 * UndoEntry
 *
 * Representa uma entrada que foi deletada e pode ser restaurada.
 * O arquivo é movido para um diretório temporário (~/.fm_trash/)
 * em vez de ser permanentemente removido.
 *
 *   trash_path   caminho onde o arquivo foi guardado no trash
 *   origin_path  caminho original de onde o arquivo veio
 */
struct UndoEntry
{
    fs::path trash_path;
    fs::path origin_path;
};

/*
 * UndoStack
 *
 * Pilha de operações de deleção que podem ser desfeitas com 'u'.
 * Cada deleção empilha um UndoEntry. O undo desempilha o topo
 * e move o arquivo de volta ao caminho original.
 *
 * O diretório de trash é criado na primeira deleção.
 */
struct UndoStack
{
    std::vector < UndoEntry > stack;

    fs::path trash_dir ( )
    {
        const char * home = getenv ( "HOME" );
        fs::path dir = home ? fs::path ( home ) / ".fm_trash" : fs::path ( "/tmp/.fm_trash" );

        std::error_code ec;
        fs::create_directories ( dir, ec );
        return dir;
    }

    /*
     * push_delete
     *
     * Move entry de origin para o trash e empilha o UndoEntry.
     * Retorna true se a operação foi concluída com sucesso.
     *
     * Parâmetros:
     *   origin  caminho completo da entrada a deletar
     */
    bool push_delete ( const fs::path & origin )
    {
        fs::path tdir = trash_dir ( );

        /* nome único no trash para evitar colisões */
        fs::path dst = tdir / origin.filename ( );
        int suffix = 0;
        while ( fs::exists ( dst ) )
        {
            dst = tdir / ( origin.filename ( ).string ( )
                           + "." + std::to_string ( ++ suffix ) );
        }

        std::error_code ec;
        fs::rename ( origin, dst, ec );

        if ( ec )
        {
            /* rename entre filesystems falha — tenta copy + remove */
            fs::copy_options opts =
                fs::copy_options::recursive |
                fs::copy_options::copy_symlinks;

            fs::copy ( origin, dst, opts, ec );
            if ( ec ) return false;

            fs::remove_all ( origin, ec );
            if ( ec ) return false;
        }

        stack.push_back ( { dst, origin } );
        return true;
    }

    /*
     * pop_undo
     *
     * Restaura a última entrada deletada ao seu caminho original.
     * Retorna true se restaurado com sucesso, false se a pilha
     * estiver vazia ou o destino já existir.
     */
    bool pop_undo ( )
    {
        if ( stack.empty ( ) ) return false;

        UndoEntry top = stack.back ( );
        stack.pop_back ( );

        if ( fs::exists ( top.origin_path ) ) return false;

        std::error_code ec;
        fs::rename ( top.trash_path, top.origin_path, ec );

        if ( ec )
        {
            fs::copy_options opts =
                fs::copy_options::recursive |
                fs::copy_options::copy_symlinks;

            fs::copy ( top.trash_path, top.origin_path, opts, ec );
            if ( ec ) return false;

            fs::remove_all ( top.trash_path, ec );
        }

        return ! ec;
    }

    bool empty ( ) const { return stack.empty ( ); }
};

/*
 * Selection
 *
 * Conjunto de entradas marcadas para operações em lote (mover).
 * O usuário seleciona com Space e executa com 'M' (Shift+M).
 * Entradas selecionadas são exibidas com prefixo [*D] ou [*F].
 * ESC limpa a seleção sem mover nada.
 */
struct Selection
{
    std::set < std::string > names;

    void toggle ( const std::string & name )
    {
        if ( names.count ( name ) )
            names.erase ( name );
        else
            names.insert ( name );
    }

    bool has ( const std::string & name ) const
    {
        return names.count ( name ) > 0;
    }

    void clear ( ) { names.clear ( ); }
    bool empty ( ) const { return names.empty ( ); }
    int  size  ( ) const { return ( int ) names.size ( ); }
};

/*
 * KeySeq
 *
 * Gerencia sequências de duas teclas no estilo vim (ex: dd, yy, yc, pp).
 * Armazena a primeira tecla pressionada e o timestamp, descartando
 * a sequência se a segunda tecla demorar mais de TIMEOUT_MS milissegundos.
 *
 * Mapa de sequências reconhecidas:
 *   yc  → SEQ_YC  copia conteúdo do arquivo para clipboard do sistema e interno
 *   yy  → SEQ_YY  copia path da entrada para o clipboard do sistema
 *   pp  → SEQ_PP  cola conteúdo copiado via yc como novo arquivo
 *   dd  → SEQ_DD  move a entrada selecionada para o trash
 *
 * Teclas que NÃO participam de sequência (chegam direto ao loop):
 *   a   cria novo arquivo
 *   A   cria nova pasta
 *   r   renomeia entrada
 *   u   desfaz última deleção
 *   /   busca por prefixo
 *   s   cicla modo de ordenação
 *   P   copia path atual para o clipboard do sistema
 *   C   clona entrada selecionada
 *   M   move seleção para destino
 *   q   encerra o programa
 */
struct KeySeq
{
    static constexpr int SEQ_YC = -1;   /* yc → copiar conteúdo do arquivo */
    static constexpr int SEQ_DD = -2;   /* dd → mover para trash            */
    static constexpr int SEQ_YY = -3;   /* yy → copiar caminho da entrada   */
    static constexpr int SEQ_PP = -4;   /* pp → colar                       */

    static constexpr int TIMEOUT_MS = 400;

    using Clock = std::chrono::steady_clock;

    int  first     = 0;
    bool waiting   = false;
    Clock::time_point last_time;

    /*
     * push
     *
     * Recebe uma tecla e retorna:
     *   0          → aguardando o segundo caractere da sequência
     *   SEQ_*      → sequência de dois caracteres completa
     *   key        → tecla avulsa (não inicia sequência ou timeout expirou)
     *
     * Apenas 'y', 'p' e 'd' iniciam sequências. As teclas 'a', 'A', 'r',
     * 'u', '/', 's', 'P', 'C', 'M' e 'q' passam direto sem espera.
     */
    int push ( int key )
    {
        auto now = Clock::now ( );

        if ( waiting )
        {
            auto elapsed = std::chrono::duration_cast < std::chrono::milliseconds >
                ( now - last_time ).count ( );

            bool timed_out = ( elapsed > TIMEOUT_MS );
            bool same_key  = ( key == first );

            waiting = false;

            if ( ! timed_out && same_key )
            {
                switch ( first )
                {
                    case 'd' : return SEQ_DD;
                    case 'p' : return SEQ_PP;
                    default  : return key;
                }
            }

            /* sequência incompleta ou timeout: verifica combinações heterogêneas */
            if ( ! timed_out && first == 'y' )
            {
                if ( key == 'y' ) return SEQ_YY;
                if ( key == 'c' ) return SEQ_YC;
            }

            return key;
        }

        /* teclas que iniciam sequência */
        if ( key == 'y' || key == 'p' || key == 'd' )
        {
            first     = key;
            waiting   = true;
            last_time = now;
            return 0;
        }

        return key;
    }
};

/*
 * ncurses_init
 */
static void ncurses_init ( )
{
    initscr ( );
    noecho ( );
    cbreak ( );
    keypad ( stdscr, TRUE );
    curs_set ( 0 );
    timeout ( 100 );
}

/*
 * prompt_input
 *
 * Exibe uma linha de input no rodapé da tela e captura uma string.
 * Suporta backspace e ESC para cancelar.
 */
static std::string prompt_input ( const std::string & label )
{
    int rows, cols;
    getmaxyx ( stdscr, rows, cols );

    const int row = rows - 1;

    echo ( );
    curs_set ( 1 );
    timeout ( -1 );

    std::string input;
    int ch;

    while ( true )
    {
        move ( row, 0 );
        clrtoeol ( );
        mvprintw ( row, 0, "%s%s", label.c_str ( ), input.c_str ( ) );
        refresh ( );

        ch = getch ( );

        if ( ch == 27 )
        {
            input.clear ( );
            break;
        }

        if ( ch == '\n' || ch == KEY_ENTER )
            break;

        if ( ( ch == KEY_BACKSPACE || ch == 127 || ch == 8 ) && ! input.empty ( ) )
            input.pop_back ( );
        else if ( ch >= 32 && ch < 127 )
            input += static_cast < char > ( ch );
    }

    noecho ( );
    curs_set ( 0 );
    timeout ( 100 );

    return input;
}

/*
 * open_editor
 *
 * Suspende o ncurses e abre o arquivo no editor ($EDITOR ou vim).
 */
static void open_editor ( const fs::path & file )
{
    const char * editor = getenv ( "EDITOR" );
    if ( ! editor )
        editor = "vim";

    std::string path = file.string ( );

    def_prog_mode ( );
    endwin ( );

    pid_t pid = fork ( );

    if ( pid == 0 )
    {
        char * args [ ] =
        {
            ( char * ) editor,
            ( char * ) path.c_str ( ),
            NULL
        };
        execvp ( editor, args );
        exit ( 1 );
    }
    if ( pid > 0 )
        waitpid ( pid, NULL, 0 );

    reset_prog_mode ( );
    refresh ( );
}

/*
 * create_file
 *
 * Solicita um nome ao usuário e cria um arquivo vazio no diretório atual.
 * Ativado pela tecla 'a' (add file).
 * Retorna false se o nome estiver vazio ou o arquivo já existir.
 */
static bool create_file ( const fs::path & path )
{
    std::string name = prompt_input ( "Novo arquivo: " );
    if ( name.empty ( ) ) return false;

    fs::path target = path / name;
    if ( fs::exists ( target ) ) return false;

    std::ofstream f ( target );
    return f.good ( );
}

/*
 * create_dir
 *
 * Solicita um nome ao usuário e cria um diretório no diretório atual.
 * Ativado pela tecla 'A' (Add directory).
 * Retorna false se o nome estiver vazio ou o diretório já existir.
 */
static bool create_dir ( const fs::path & path )
{
    std::string name = prompt_input ( "Nova pasta: " );
    if ( name.empty ( ) ) return false;

    fs::path target = path / name;
    if ( fs::exists ( target ) ) return false;

    return fs::create_directory ( target );
}

/*
 * rename_entry
 */
static bool rename_entry ( const fs::path & path, const Entry * entry )
{
    std::string new_name = prompt_input ( "Renomear para: " );
    if ( new_name.empty ( ) ) return false;

    fs::path src = path / entry -> getName ( );
    fs::path dst = path / new_name;

    if ( fs::exists ( dst ) ) return false;

    fs::rename ( src, dst );
    return true;
}

/*
 * copy_to_system_clipboard
 *
 * Envia uma string para o clipboard do sistema.
 * Tenta wl-copy (Wayland) primeiro; cai para xclip (X11) se falhar.
 *
 * Parâmetros:
 *   value  string a ser copiada
 */
static void copy_to_system_clipboard ( const std::string & value )
{
    std::string cmd =
        "printf '%s' '" + value + "' | wl-copy 2>/dev/null || "
        "printf '%s' '" + value + "' | xclip -selection clipboard 2>/dev/null";
    system ( cmd.c_str ( ) );
}

/*
 * cmd_yc
 *
 * Copia o conteúdo do arquivo selecionado para o clipboard do sistema
 * e para o clipboard interno (usado pelo pp para colar dentro do fm).
 * Ativado pela sequência 'yc' (yank content).
 * Só funciona para entradas do tipo ENTRY_FILE.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 *   clip   clipboard interno a ser preenchido
 *
 * Retorno:
 *   true se o conteúdo foi lido com sucesso
 */
static bool cmd_yc ( const fs::path & path, const Entry * entry, Clipboard & clip )
{
    if ( entry -> getType ( ) != ENTRY_FILE )
        return false;

    fs::path src = path / entry -> getName ( );
    std::ifstream f ( src );
    if ( ! f.is_open ( ) ) return false;

    std::ostringstream ss;
    ss << f.rdbuf ( );

    clip.content = ss.str ( );
    clip.src     = src;
    clip.mode    = CLIP_CONTENT;

    /* espelha no clipboard do sistema para uso fora do fm */
    copy_to_system_clipboard ( clip.content );

    return true;
}

/*
 * cmd_yy
 *
 * Copia o path completo da entrada selecionada para o clipboard do sistema.
 * Ativado pela sequência 'yy' (yank path).
 * Funciona para arquivos e pastas.
 * Não altera o clipboard interno — yy e pp são independentes.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 */
static void cmd_yy ( const fs::path & path, const Entry * entry )
{
    copy_to_system_clipboard ( ( path / entry -> getName ( ) ).string ( ) );
}

/*
 * cmd_pp
 *
 * Cola o conteúdo copiado via yc como um novo arquivo no diretório atual.
 * Ativado pela sequência 'pp' (paste).
 *
 * Solicita o nome do arquivo de destino via prompt. Cancela se o nome
 * estiver vazio ou o destino já existir.
 *
 * Parâmetros:
 *   path  diretório de destino
 *   clip  clipboard interno (deve estar no modo CLIP_CONTENT)
 *
 * Retorno:
 *   true se o arquivo foi criado com sucesso
 */
static bool cmd_pp ( const fs::path & path, const Clipboard & clip )
{
    if ( clip.mode == CLIP_EMPTY )
        return false;

    std::string name = prompt_input ( "Colar como: " );
    if ( name.empty ( ) ) return false;

    fs::path dst = path / name;
    if ( fs::exists ( dst ) ) return false;

    std::ofstream f ( dst );
    if ( ! f.is_open ( ) ) return false;
    f << clip.content;
    return f.good ( );
}

/*
 * cmd_dd
 *
 * Move a entrada para o trash (via UndoStack) após confirmação.
 * Ativado pela sequência 'dd' (delete).
 * O arquivo pode ser restaurado com 'u'.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 *   undo   pilha de undo onde a operação será empilhada
 *
 * Retorno:
 *   true  se movido para o trash com sucesso
 *   false se cancelado ou operação falhar
 */
static bool cmd_dd ( const fs::path & path, const Entry * entry, UndoStack & undo )
{
    std::string confirm = prompt_input (
        "Deletar \"" + entry -> getName ( ) + "\"? (s/n): "
    );

    if ( confirm != "s" && confirm != "S" )
        return false;

    fs::path target = path / entry -> getName ( );
    return undo.push_delete ( target );
}

/*
 * copy_current_path
 *
 * Copia o caminho do diretório atual para o clipboard do sistema.
 * Ativado pela tecla 'P'.
 */
static void copy_current_path ( const fs::path & path )
{
    copy_to_system_clipboard ( path.string ( ) );
}

/*
 * clone_entry
 *
 * Cria uma cópia da entrada selecionada no mesmo diretório.
 * Ativado pela tecla 'C'. O sufixo "_copy" é adicionado ao nome;
 * se já existir, incrementa com "_copy_N".
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada a clonar
 *
 * Retorno:
 *   true se a cópia foi criada com sucesso
 */
static bool clone_entry ( const fs::path & path, const Entry * entry )
{
    fs::path src = path / entry -> getName ( );
    fs::path dst = path / ( entry -> getName ( ) + "_copy" );
    int i = 1;
    while ( fs::exists ( dst ) )
        dst = path / ( entry -> getName ( ) + "_copy_" + std::to_string ( i ++ ) );
    std::error_code ec;
    fs::copy ( src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec );
    return ! ec;
}

/*
 * cmd_move_selection
 *
 * Move todos os arquivos/pastas selecionados para um diretório de destino.
 * Ativado por 'M' (Shift+M). Solicita o caminho destino via prompt.
 * Suporta expansão de '~' para o diretório home do usuário.
 * Cria o diretório destino se não existir.
 * Não sobrescreve entradas já existentes no destino.
 * Limpa a seleção após mover com sucesso.
 *
 * Parâmetros:
 *   path  diretório atual (origem)
 *   sel   conjunto de entradas selecionadas
 *
 * Retorno:
 *   true se pelo menos uma entrada foi movida com sucesso
 */
static bool cmd_move_selection ( const fs::path & path, Selection & sel )
{
    if ( sel.empty ( ) ) return false;

    std::string dest_str = prompt_input ( "Mover para: " );
    if ( dest_str.empty ( ) ) return false;

    fs::path dest = dest_str;

    /* expande ~ para o home do usuário */
    if ( ! dest_str.empty ( ) && dest_str [ 0 ] == '~' )
    {
        const char * home = getenv ( "HOME" );
        if ( home )
            dest = fs::path ( home ) / dest_str.substr ( 1 );
    }

    std::error_code ec;

    /* cria o diretório destino se não existir */
    if ( ! fs::exists ( dest ) )
        fs::create_directories ( dest, ec );

    if ( ! fs::is_directory ( dest ) ) return false;

    bool any_moved = false;

    for ( const auto & name : sel.names )
    {
        fs::path src = path / name;
        fs::path dst = dest / name;

        if ( ! fs::exists ( src ) ) continue;

        /* não sobrescreve entrada já existente no destino */
        if ( fs::exists ( dst ) ) continue;

        fs::rename ( src, dst, ec );

        if ( ec )
        {
            /* fallback para cross-filesystem: copy + remove */
            fs::copy ( src, dst,
                       fs::copy_options::recursive |
                       fs::copy_options::copy_symlinks, ec );
            if ( ! ec )
            {
                fs::remove_all ( src, ec );
                any_moved = true;
            }
        }
        else
        {
            any_moved = true;
        }
    }

    if ( any_moved )
        sel.clear ( );

    return any_moved;
}

/*
 * sort_label
 *
 * Retorna uma string curta descrevendo o modo de ordenação atual.
 */
static const char * sort_label ( SortMode mode )
{
    switch ( mode )
    {
        case SORT_NAME : return "nome";
        case SORT_SIZE : return "tamanho";
        case SORT_DATE : return "data";
    }
    return "";
}

/*
 * clip_status
 *
 * Retorna uma string de status do clipboard interno para exibição no rodapé.
 * Exibe [yc] e o nome do arquivo quando há conteúdo disponível para pp.
 * Vazio se não houver nada copiado.
 */
static std::string clip_status ( const Clipboard & clip )
{
    if ( clip.mode == CLIP_EMPTY ) return "";
    return "[yc] " + clip.src.filename ( ).string ( );
}

/*
 * render
 *
 * Redesenha toda a interface usando flatten() — O(n) total,
 * eliminando o custo O(n²) do loop original com get(i).
 *
 * Entradas com match de busca ativa são destacadas com A_BOLD.
 * A entrada selecionada usa A_REVERSE.
 * Entradas marcadas para mover exibem prefixo [*D] ou [*F].
 *
 * Parâmetros:
 *   m            referência ao objeto Methods
 *   cursor       índice da entrada selecionada
 *   path         caminho atual
 *   clip         clipboard atual
 *   undo         pilha de undo (para indicar se há undo disponível)
 *   sel          seleção atual para mover
 *   search_name  nome da entrada buscada (vazio = sem busca ativa)
 */
static void render
(
    const Methods     & m,
    int                 cursor,
    const fs::path    & path,
    const Clipboard   & clip,
    const UndoStack   & undo,
    const Selection   & sel,
    const std::string & search_name = ""
)
{
    clear ( );

    int rows, cols;
    getmaxyx ( stdscr, rows, cols );

    const int list_rows = rows - 5; /* linhas disponíveis para a listagem */

    /* — flatten: O(n) em vez de O(n²) — */
    Entry * flat [ FM_MAX_ENTRIES ];
    int n = m.flatten ( ( Entry ** ) flat );

    if ( n == 0 )
    {
        mvprintw ( 0, 0, "(diretório vazio)" );
    }
    else
    {
        /* scroll: garante que o cursor fique sempre visível */
        int scroll = 0;
        if ( cursor >= list_rows )
            scroll = cursor - list_rows + 1;

        int display_end = scroll + list_rows;
        if ( display_end > n ) display_end = n;

        for ( int i = scroll; i < display_end; i ++ )
        {
            int row = i - scroll;

            bool is_cursor   = ( i == cursor );
            bool is_match    = ( ! search_name.empty ( )
                                 && flat [ i ] -> getName ( ) == search_name );
            bool is_selected = sel.has ( flat [ i ] -> getName ( ) );

            if ( is_cursor   ) attron  ( A_REVERSE );
            if ( is_match    ) attron  ( A_BOLD    );
            if ( is_selected ) attron  ( A_BOLD    );

            /* prefixo: [*D]/[*F] se selecionado, [D]/[F] caso contrário */
            const char * prefix;
            if ( is_selected )
                prefix = ( flat [ i ] -> getType ( ) == ENTRY_DIR ) ? "[*D]" : "[*F]";
            else
                prefix = ( flat [ i ] -> getType ( ) == ENTRY_DIR ) ? "[D] " : "[F] ";

            mvprintw ( row, 0, "%s%s", prefix, flat [ i ] -> getName ( ).c_str ( ) );

            if ( is_selected ) attroff ( A_BOLD    );
            if ( is_match    ) attroff ( A_BOLD    );
            if ( is_cursor   ) attroff ( A_REVERSE );
        }
    }

    /* — rodapé — */
    mvprintw ( rows - 5, 0, "%-*s", cols, path.c_str ( ) );

    /* status: clipboard + seleção + undo + ordenação */
    std::string status;
    std::string cs = clip_status ( clip );
    if ( ! cs.empty ( )   ) status += cs + "  ";
    if ( ! sel.empty ( )  ) status += "[sel: " + std::to_string ( sel.size ( ) ) + "]  ";
    if ( ! undo.empty ( ) ) status += "[u: desfazer]  ";
    status += std::string ( "ord: " ) + sort_label ( m.getSortMode ( ) );

    mvprintw ( rows - 4, 0, "%-*s", cols, status.c_str ( ) );

    /* busca ativa */
    if ( ! search_name.empty ( ) )
        mvprintw ( rows - 3, 0, "busca: %s", search_name.c_str ( ) );
    else
        mvprintw ( rows - 3, 0, "%-*s", cols, "" );

    /* mapa de teclas */
    mvprintw
    (
        rows - 2,
        0,
        "Enter-abrir  Bksp-voltar  yc-copiar conteudo  yy-copiar entrada  "
        "pp-colar  dd-deletar  u-desfazer  a-criar arq  A-criar pasta  "
        "r-renomear  /-buscar  s-ordenar  P-copiar path  C-clonar  "
        "Spc-selecionar  M-mover selecao  ESC-limpar sel  q-sair"
    );

    refresh ( );
}

/*
 * cmd_search
 *
 * Solicita um prefixo ao usuário e usa search_prefix da BST
 * para localizar entradas. Retorna o índice in-order do primeiro
 * resultado, ou -1 se não encontrado.
 *
 * A busca tenta primeiro ENTRY_DIR, depois ENTRY_FILE.
 * O nome do primeiro match é devolvido em result_name para
 * que o render possa destacá-lo.
 *
 * Complexidade da busca: O(log n + k)
 *
 * Parâmetros:
 *   m            referência ao Methods
 *   result_name  saída: nome da primeira entrada encontrada
 *
 * Retorno:
 *   índice in-order do primeiro resultado, ou -1 se não encontrado
 */
static int cmd_search ( const Methods & m, std::string & result_name )
{
    std::string prefix = prompt_input ( "/buscar: " );
    result_name.clear ( );

    if ( prefix.empty ( ) )
        return -1;

    Entry * results [ FM_MAX_ENTRIES ];
    int found = m.search_prefix ( prefix, ( Entry ** ) results, FM_MAX_ENTRIES );

    if ( found == 0 )
        return -1;

    result_name = results [ 0 ] -> getName ( );
    return m.find_index ( result_name );
}

/*
 * main
 *
 * Loop principal do navegador de arquivos.
 *
 * Mapa completo de teclas e sequências:
 *
 *   Navegação:
 *     KEY_DOWN         desce o cursor (circular)
 *     KEY_UP           sobe o cursor (circular)
 *     Enter            entra em pasta / abre arquivo no editor
 *     Backspace        sobe um nível no diretório
 *
 *   Clipboard:
 *     yc               copia conteúdo do arquivo → clipboard do sistema + interno (pp)
 *     yy               copia path da entrada → clipboard do sistema apenas
 *     pp               cola conteúdo de yc como novo arquivo no dir atual
 *     dd               move para trash (desfazível com u)
 *
 *   Seleção e mover:
 *     Space            seleciona/deseleciona entrada sob o cursor e avança cursor
 *     M                move todas as entradas selecionadas para destino informado
 *     ESC              limpa a seleção sem mover
 *
 *   Edição (teclas avulsas, chegam direto ao loop):
 *     a                cria novo arquivo (add file)
 *     A                cria nova pasta (Add directory)
 *     r                renomeia entrada selecionada
 *     u                desfaz última deleção (restaura do trash)
 *
 *   Utilitários:
 *     /                busca por prefixo na BST — O(log n + k)
 *     s                cicla modo de ordenação (nome → tamanho → data)
 *     P                copia path do diretório atual para clipboard do sistema
 *     C                clona arquivo/pasta selecionado no mesmo diretório
 *     q                encerra o programa
 */
int main ( int argc, char ** argv )
{
    fs::path   path = fs::current_path ( );
    Methods    m ( FM_MAX_ENTRIES );
    Clipboard  clip;
    KeySeq     seq;
    UndoStack  undo;
    Selection  sel;

    std::string search_name; /* nome do último resultado de busca */

    m.load ( path );

    ncurses_init ( );

    int cursor = 0;
    int key;

    render ( m, cursor, path, clip, undo, sel, search_name );

    while ( true )
    {
        key = getch ( );

        if ( key == ERR )
        {
            render ( m, cursor, path, clip, undo, sel, search_name );
            continue;
        }

        int cmd = seq.push ( key );

        if ( cmd == 0 )
            continue;

        if ( cmd == 'q' )
            break;

        if ( cmd == 'P' )
            copy_current_path ( path );

        const int count = m.count ( );

        if ( cmd == 'C' && count > 0 )
        {
            clone_entry ( path, m.get ( cursor ) );
            m.load ( path );
        }

        /* — navegação — */
        if ( cmd == KEY_DOWN )
        {
            cursor = ( count > 0 ) ? ( cursor + 1 ) % count : 0;
            search_name.clear ( ); /* limpa destaque ao mover */
        }

        if ( cmd == KEY_UP )
        {
            cursor = ( count > 0 ) ? ( cursor - 1 + count ) % count : 0;
            search_name.clear ( );
        }

        /* — entrar em pasta / abrir arquivo — */
        if ( ( cmd == KEY_ENTER || cmd == '\n' ) && count > 0 )
        {
            auto entry = m.get ( cursor );

            if ( entry -> getType ( ) == ENTRY_DIR )
            {
                fs::path next = path / entry -> getName ( );
                if ( fs::exists ( next ) && fs::is_directory ( next ) )
                {
                    path = fs::canonical ( next );
                    m.load ( path );
                    cursor = 0;
                    search_name.clear ( );
                    sel.clear ( ); /* limpa seleção ao trocar de diretório */
                }
            }
            else if ( entry -> getType ( ) == ENTRY_FILE )
            {
                open_editor ( path / entry -> getName ( ) );
                m.load ( path );
            }
        }

        /* — voltar um nível — */
        if ( cmd == KEY_BACKSPACE || cmd == 127 || cmd == 8 )
        {
            path = path.parent_path ( );
            m.load ( path );
            cursor = 0;
            search_name.clear ( );
            sel.clear ( ); /* limpa seleção ao trocar de diretório */
        }

        /* — criar arquivo — */
        if ( cmd == 'a' )
        {
            create_file ( path );
            m.load ( path );
        }

        /* — criar pasta — */
        if ( cmd == 'A' )
        {
            create_dir ( path );
            m.load ( path );
        }

        /* — renomear — */
        if ( cmd == 'r' && count > 0 )
        {
            rename_entry ( path, m.get ( cursor ) );
            m.load ( path );
            search_name.clear ( );
        }

        /* — yc: copiar conteúdo do arquivo — */
        if ( cmd == KeySeq::SEQ_YC && count > 0 )
            cmd_yc ( path, m.get ( cursor ), clip );

        /* — yy: copiar path da entrada para clipboard do sistema — */
        if ( cmd == KeySeq::SEQ_YY && count > 0 )
            cmd_yy ( path, m.get ( cursor ) );

        /* — pp: colar — */
        if ( cmd == KeySeq::SEQ_PP )
        {
            cmd_pp ( path, clip );
            m.load ( path );
        }

        /* — dd: mover para trash — */
        if ( cmd == KeySeq::SEQ_DD && count > 0 )
        {
            if ( cmd_dd ( path, m.get ( cursor ), undo ) )
            {
                m.load ( path );
                if ( cursor >= m.count ( ) && cursor > 0 )
                    cursor --;
                search_name.clear ( );
            }
        }

        /* — u: desfazer última deleção — */
        if ( cmd == 'u' )
        {
            if ( undo.pop_undo ( ) )
            {
                m.load ( path );
                search_name.clear ( );
            }
        }

        /* — /: busca por prefixo na BST — */
        if ( cmd == '/' )
        {
            int idx = cmd_search ( m, search_name );
            if ( idx >= 0 )
                cursor = idx;
            /* se não encontrado, search_name fica vazio e render mostra nada */
        }

        /* — s: ciclar modo de ordenação — */
        if ( cmd == 's' )
        {
            SortMode next;
            switch ( m.getSortMode ( ) )
            {
                case SORT_NAME : next = SORT_SIZE; break;
                case SORT_SIZE : next = SORT_DATE; break;
                case SORT_DATE : next = SORT_NAME; break;
                default        : next = SORT_NAME;
            }
            m.setSortMode ( next, path );
            cursor = 0;
            search_name.clear ( );
        }

        /* — Space: selecionar/deselecionar entrada e avançar cursor — */
        if ( cmd == ' ' && count > 0 )
        {
            sel.toggle ( m.get ( cursor ) -> getName ( ) );
            /* avança cursor automaticamente para agilizar seleção em sequência */
            cursor = ( cursor + 1 ) % count;
        }

        /* — M: mover seleção para destino — */
        if ( cmd == 'M' && ! sel.empty ( ) )
        {
            if ( cmd_move_selection ( path, sel ) )
            {
                m.load ( path );
                if ( cursor >= m.count ( ) && cursor > 0 )
                    cursor --;
                search_name.clear ( );
            }
        }

        /* — ESC: limpar seleção — */#include "Methods.hpp"
#include <ncurses.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>
#include <set>

/*
 * ClipboardMode
 *
 * Indica o que está armazenado no clipboard interno no momento:
 *   CLIP_EMPTY    → nada copiado ainda
 *   CLIP_CONTENT  → conteúdo de um arquivo (yc)
 *
 * Nota: yy copia o path direto para o clipboard do sistema e não
 * utiliza o clipboard interno — por isso CLIP_PATH foi removido.
 */
enum ClipboardMode
{
    CLIP_EMPTY,
    CLIP_CONTENT
};

/*
 * Clipboard
 *
 * Estrutura que representa o estado global do clipboard interno.
 * Usado exclusivamente pelo par yc/pp para copiar e colar conteúdo
 * de arquivo dentro do fm.
 *
 *   mode     indica se há conteúdo disponível para colar
 *   content  conteúdo bruto do arquivo copiado via yc
 *   src      caminho original do arquivo copiado (exibido no rodapé)
 */
struct Clipboard
{
    ClipboardMode  mode    = CLIP_EMPTY;
    std::string    content = "";
    fs::path       src     = "";
};

/*
 * UndoEntry
 *
 * Representa uma entrada que foi deletada e pode ser restaurada.
 * O arquivo é movido para um diretório temporário (~/.fm_trash/)
 * em vez de ser permanentemente removido.
 *
 *   trash_path   caminho onde o arquivo foi guardado no trash
 *   origin_path  caminho original de onde o arquivo veio
 */
struct UndoEntry
{
    fs::path trash_path;
    fs::path origin_path;
};

/*
 * UndoStack
 *
 * Pilha de operações de deleção que podem ser desfeitas com 'u'.
 * Cada deleção empilha um UndoEntry. O undo desempilha o topo
 * e move o arquivo de volta ao caminho original.
 *
 * O diretório de trash é criado na primeira deleção.
 */
struct UndoStack
{
    std::vector < UndoEntry > stack;

    fs::path trash_dir ( )
    {
        const char * home = getenv ( "HOME" );
        fs::path dir = home ? fs::path ( home ) / ".fm_trash" : fs::path ( "/tmp/.fm_trash" );

        std::error_code ec;
        fs::create_directories ( dir, ec );
        return dir;
    }

    /*
     * push_delete
     *
     * Move entry de origin para o trash e empilha o UndoEntry.
     * Retorna true se a operação foi concluída com sucesso.
     *
     * Parâmetros:
     *   origin  caminho completo da entrada a deletar
     */
    bool push_delete ( const fs::path & origin )
    {
        fs::path tdir = trash_dir ( );

        /* nome único no trash para evitar colisões */
        fs::path dst = tdir / origin.filename ( );
        int suffix = 0;
        while ( fs::exists ( dst ) )
        {
            dst = tdir / ( origin.filename ( ).string ( )
                           + "." + std::to_string ( ++ suffix ) );
        }

        std::error_code ec;
        fs::rename ( origin, dst, ec );

        if ( ec )
        {
            /* rename entre filesystems falha — tenta copy + remove */
            fs::copy_options opts =
                fs::copy_options::recursive |
                fs::copy_options::copy_symlinks;

            fs::copy ( origin, dst, opts, ec );
            if ( ec ) return false;

            fs::remove_all ( origin, ec );
            if ( ec ) return false;
        }

        stack.push_back ( { dst, origin } );
        return true;
    }

    /*
     * pop_undo
     *
     * Restaura a última entrada deletada ao seu caminho original.
     * Retorna true se restaurado com sucesso, false se a pilha
     * estiver vazia ou o destino já existir.
     */
    bool pop_undo ( )
    {
        if ( stack.empty ( ) ) return false;

        UndoEntry top = stack.back ( );
        stack.pop_back ( );

        if ( fs::exists ( top.origin_path ) ) return false;

        std::error_code ec;
        fs::rename ( top.trash_path, top.origin_path, ec );

        if ( ec )
        {
            fs::copy_options opts =
                fs::copy_options::recursive |
                fs::copy_options::copy_symlinks;

            fs::copy ( top.trash_path, top.origin_path, opts, ec );
            if ( ec ) return false;

            fs::remove_all ( top.trash_path, ec );
        }

        return ! ec;
    }

    bool empty ( ) const { return stack.empty ( ); }
};

/*
 * Selection
 *
 * Conjunto de entradas marcadas para operações em lote (mover).
 * O usuário seleciona com Space e executa com 'M' (Shift+M).
 * Entradas selecionadas são exibidas com prefixo [*D] ou [*F].
 * ESC limpa a seleção sem mover nada.
 */
struct Selection
{
    std::set < std::string > names;

    void toggle ( const std::string & name )
    {
        if ( names.count ( name ) )
            names.erase ( name );
        else
            names.insert ( name );
    }

    bool has ( const std::string & name ) const
    {
        return names.count ( name ) > 0;
    }

    void clear ( ) { names.clear ( ); }
    bool empty ( ) const { return names.empty ( ); }
    int  size  ( ) const { return ( int ) names.size ( ); }
};

/*
 * KeySeq
 *
 * Gerencia sequências de duas teclas no estilo vim (ex: dd, yy, yc, pp).
 * Armazena a primeira tecla pressionada e o timestamp, descartando
 * a sequência se a segunda tecla demorar mais de TIMEOUT_MS milissegundos.
 *
 * Mapa de sequências reconhecidas:
 *   yc  → SEQ_YC  copia conteúdo do arquivo para clipboard do sistema e interno
 *   yy  → SEQ_YY  copia path da entrada para o clipboard do sistema
 *   pp  → SEQ_PP  cola conteúdo copiado via yc como novo arquivo
 *   dd  → SEQ_DD  move a entrada selecionada para o trash
 *
 * Teclas que NÃO participam de sequência (chegam direto ao loop):
 *   a   cria novo arquivo
 *   A   cria nova pasta
 *   r   renomeia entrada
 *   u   desfaz última deleção
 *   /   busca por prefixo
 *   s   cicla modo de ordenação
 *   P   copia path atual para o clipboard do sistema
 *   C   clona entrada selecionada
 *   M   move seleção para destino
 *   q   encerra o programa
 */
struct KeySeq
{
    static constexpr int SEQ_YC = -1;   /* yc → copiar conteúdo do arquivo */
    static constexpr int SEQ_DD = -2;   /* dd → mover para trash            */
    static constexpr int SEQ_YY = -3;   /* yy → copiar caminho da entrada   */
    static constexpr int SEQ_PP = -4;   /* pp → colar                       */

    static constexpr int TIMEOUT_MS = 400;

    using Clock = std::chrono::steady_clock;

    int  first     = 0;
    bool waiting   = false;
    Clock::time_point last_time;

    /*
     * push
     *
     * Recebe uma tecla e retorna:
     *   0          → aguardando o segundo caractere da sequência
     *   SEQ_*      → sequência de dois caracteres completa
     *   key        → tecla avulsa (não inicia sequência ou timeout expirou)
     *
     * Apenas 'y', 'p' e 'd' iniciam sequências. As teclas 'a', 'A', 'r',
     * 'u', '/', 's', 'P', 'C', 'M' e 'q' passam direto sem espera.
     */
    int push ( int key )
    {
        auto now = Clock::now ( );

        if ( waiting )
        {
            auto elapsed = std::chrono::duration_cast < std::chrono::milliseconds >
                ( now - last_time ).count ( );

            bool timed_out = ( elapsed > TIMEOUT_MS );
            bool same_key  = ( key == first );

            waiting = false;

            if ( ! timed_out && same_key )
            {
                switch ( first )
                {
                    case 'd' : return SEQ_DD;
                    case 'p' : return SEQ_PP;
                    default  : return key;
                }
            }

            /* sequência incompleta ou timeout: verifica combinações heterogêneas */
            if ( ! timed_out && first == 'y' )
            {
                if ( key == 'y' ) return SEQ_YY;
                if ( key == 'c' ) return SEQ_YC;
            }

            return key;
        }

        /* teclas que iniciam sequência */
        if ( key == 'y' || key == 'p' || key == 'd' )
        {
            first     = key;
            waiting   = true;
            last_time = now;
            return 0;
        }

        return key;
    }
};

/*
 * ncurses_init
 */
static void ncurses_init ( )
{
    initscr ( );
    noecho ( );
    cbreak ( );
    keypad ( stdscr, TRUE );
    curs_set ( 0 );
    timeout ( 100 );
}

/*
 * prompt_input
 *
 * Exibe uma linha de input no rodapé da tela e captura uma string.
 * Suporta backspace e ESC para cancelar.
 */
static std::string prompt_input ( const std::string & label )
{
    int rows, cols;
    getmaxyx ( stdscr, rows, cols );

    const int row = rows - 1;

    echo ( );
    curs_set ( 1 );
    timeout ( -1 );

    std::string input;
    int ch;

    while ( true )
    {
        move ( row, 0 );
        clrtoeol ( );
        mvprintw ( row, 0, "%s%s", label.c_str ( ), input.c_str ( ) );
        refresh ( );

        ch = getch ( );

        if ( ch == 27 )
        {
            input.clear ( );
            break;
        }

        if ( ch == '\n' || ch == KEY_ENTER )
            break;

        if ( ( ch == KEY_BACKSPACE || ch == 127 || ch == 8 ) && ! input.empty ( ) )
            input.pop_back ( );
        else if ( ch >= 32 && ch < 127 )
            input += static_cast < char > ( ch );
    }

    noecho ( );
    curs_set ( 0 );
    timeout ( 100 );

    return input;
}

/*
 * open_editor
 *
 * Suspende o ncurses e abre o arquivo no editor ($EDITOR ou vim).
 */
static void open_editor ( const fs::path & file )
{
    const char * editor = getenv ( "EDITOR" );
    if ( ! editor )
        editor = "vim";

    std::string path = file.string ( );

    def_prog_mode ( );
    endwin ( );

    pid_t pid = fork ( );

    if ( pid == 0 )
    {
        char * args [ ] =
        {
            ( char * ) editor,
            ( char * ) path.c_str ( ),
            NULL
        };
        execvp ( editor, args );
        exit ( 1 );
    }
    if ( pid > 0 )
        waitpid ( pid, NULL, 0 );

    reset_prog_mode ( );
    refresh ( );
}

/*
 * create_file
 *
 * Solicita um nome ao usuário e cria um arquivo vazio no diretório atual.
 * Ativado pela tecla 'a' (add file).
 * Retorna false se o nome estiver vazio ou o arquivo já existir.
 */
static bool create_file ( const fs::path & path )
{
    std::string name = prompt_input ( "Novo arquivo: " );
    if ( name.empty ( ) ) return false;

    fs::path target = path / name;
    if ( fs::exists ( target ) ) return false;

    std::ofstream f ( target );
    return f.good ( );
}

/*
 * create_dir
 *
 * Solicita um nome ao usuário e cria um diretório no diretório atual.
 * Ativado pela tecla 'A' (Add directory).
 * Retorna false se o nome estiver vazio ou o diretório já existir.
 */
static bool create_dir ( const fs::path & path )
{
    std::string name = prompt_input ( "Nova pasta: " );
    if ( name.empty ( ) ) return false;

    fs::path target = path / name;
    if ( fs::exists ( target ) ) return false;

    return fs::create_directory ( target );
}

/*
 * rename_entry
 */
static bool rename_entry ( const fs::path & path, const Entry * entry )
{
    std::string new_name = prompt_input ( "Renomear para: " );
    if ( new_name.empty ( ) ) return false;

    fs::path src = path / entry -> getName ( );
    fs::path dst = path / new_name;

    if ( fs::exists ( dst ) ) return false;

    fs::rename ( src, dst );
    return true;
}

/*
 * copy_to_system_clipboard
 *
 * Envia uma string para o clipboard do sistema.
 * Tenta wl-copy (Wayland) primeiro; cai para xclip (X11) se falhar.
 *
 * Parâmetros:
 *   value  string a ser copiada
 */
static void copy_to_system_clipboard ( const std::string & value )
{
    std::string cmd =
        "printf '%s' '" + value + "' | wl-copy 2>/dev/null || "
        "printf '%s' '" + value + "' | xclip -selection clipboard 2>/dev/null";
    system ( cmd.c_str ( ) );
}

/*
 * cmd_yc
 *
 * Copia o conteúdo do arquivo selecionado para o clipboard do sistema
 * e para o clipboard interno (usado pelo pp para colar dentro do fm).
 * Ativado pela sequência 'yc' (yank content).
 * Só funciona para entradas do tipo ENTRY_FILE.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 *   clip   clipboard interno a ser preenchido
 *
 * Retorno:
 *   true se o conteúdo foi lido com sucesso
 */
static bool cmd_yc ( const fs::path & path, const Entry * entry, Clipboard & clip )
{
    if ( entry -> getType ( ) != ENTRY_FILE )
        return false;

    fs::path src = path / entry -> getName ( );
    std::ifstream f ( src );
    if ( ! f.is_open ( ) ) return false;

    std::ostringstream ss;
    ss << f.rdbuf ( );

    clip.content = ss.str ( );
    clip.src     = src;
    clip.mode    = CLIP_CONTENT;

    /* espelha no clipboard do sistema para uso fora do fm */
    copy_to_system_clipboard ( clip.content );

    return true;
}

/*
 * cmd_yy
 *
 * Copia o path completo da entrada selecionada para o clipboard do sistema.
 * Ativado pela sequência 'yy' (yank path).
 * Funciona para arquivos e pastas.
 * Não altera o clipboard interno — yy e pp são independentes.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 */
static void cmd_yy ( const fs::path & path, const Entry * entry )
{
    copy_to_system_clipboard ( ( path / entry -> getName ( ) ).string ( ) );
}

/*
 * cmd_pp
 *
 * Cola o conteúdo copiado via yc como um novo arquivo no diretório atual.
 * Ativado pela sequência 'pp' (paste).
 *
 * Solicita o nome do arquivo de destino via prompt. Cancela se o nome
 * estiver vazio ou o destino já existir.
 *
 * Parâmetros:
 *   path  diretório de destino
 *   clip  clipboard interno (deve estar no modo CLIP_CONTENT)
 *
 * Retorno:
 *   true se o arquivo foi criado com sucesso
 */
static bool cmd_pp ( const fs::path & path, const Clipboard & clip )
{
    if ( clip.mode == CLIP_EMPTY )
        return false;

    std::string name = prompt_input ( "Colar como: " );
    if ( name.empty ( ) ) return false;

    fs::path dst = path / name;
    if ( fs::exists ( dst ) ) return false;

    std::ofstream f ( dst );
    if ( ! f.is_open ( ) ) return false;
    f << clip.content;
    return f.good ( );
}

/*
 * cmd_dd
 *
 * Move a entrada para o trash (via UndoStack) após confirmação.
 * Ativado pela sequência 'dd' (delete).
 * O arquivo pode ser restaurado com 'u'.
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada selecionada
 *   undo   pilha de undo onde a operação será empilhada
 *
 * Retorno:
 *   true  se movido para o trash com sucesso
 *   false se cancelado ou operação falhar
 */
static bool cmd_dd ( const fs::path & path, const Entry * entry, UndoStack & undo )
{
    std::string confirm = prompt_input (
        "Deletar \"" + entry -> getName ( ) + "\"? (s/n): "
    );

    if ( confirm != "s" && confirm != "S" )
        return false;

    fs::path target = path / entry -> getName ( );
    return undo.push_delete ( target );
}

/*
 * copy_current_path
 *
 * Copia o caminho do diretório atual para o clipboard do sistema.
 * Ativado pela tecla 'P'.
 */
static void copy_current_path ( const fs::path & path )
{
    copy_to_system_clipboard ( path.string ( ) );
}

/*
 * clone_entry
 *
 * Cria uma cópia da entrada selecionada no mesmo diretório.
 * Ativado pela tecla 'C'. O sufixo "_copy" é adicionado ao nome;
 * se já existir, incrementa com "_copy_N".
 *
 * Parâmetros:
 *   path   diretório atual
 *   entry  entrada a clonar
 *
 * Retorno:
 *   true se a cópia foi criada com sucesso
 */
static bool clone_entry ( const fs::path & path, const Entry * entry )
{
    fs::path src = path / entry -> getName ( );
    fs::path dst = path / ( entry -> getName ( ) + "_copy" );
    int i = 1;
    while ( fs::exists ( dst ) )
        dst = path / ( entry -> getName ( ) + "_copy_" + std::to_string ( i ++ ) );
    std::error_code ec;
    fs::copy ( src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec );
    return ! ec;
}

/*
 * cmd_move_selection
 *
 * Move todos os arquivos/pastas selecionados para um diretório de destino.
 * Ativado por 'M' (Shift+M). Solicita o caminho destino via prompt.
 * Suporta expansão de '~' para o diretório home do usuário.
 * Cria o diretório destino se não existir.
 * Não sobrescreve entradas já existentes no destino.
 * Limpa a seleção após mover com sucesso.
 *
 * Parâmetros:
 *   path  diretório atual (origem)
 *   sel   conjunto de entradas selecionadas
 *
 * Retorno:
 *   true se pelo menos uma entrada foi movida com sucesso
 */
static bool cmd_move_selection ( const fs::path & path, Selection & sel )
{
    if ( sel.empty ( ) ) return false;

    std::string dest_str = prompt_input ( "Mover para: " );
    if ( dest_str.empty ( ) ) return false;

    fs::path dest = dest_str;

    /* expande ~ para o home do usuário */
    if ( ! dest_str.empty ( ) && dest_str [ 0 ] == '~' )
    {
        const char * home = getenv ( "HOME" );
        if ( home )
            dest = fs::path ( home ) / dest_str.substr ( 1 );
    }

    std::error_code ec;

    /* cria o diretório destino se não existir */
    if ( ! fs::exists ( dest ) )
        fs::create_directories ( dest, ec );

    if ( ! fs::is_directory ( dest ) ) return false;

    bool any_moved = false;

    for ( const auto & name : sel.names )
    {
        fs::path src = path / name;
        fs::path dst = dest / name;

        if ( ! fs::exists ( src ) ) continue;

        /* não sobrescreve entrada já existente no destino */
        if ( fs::exists ( dst ) ) continue;

        fs::rename ( src, dst, ec );

        if ( ec )
        {
            /* fallback para cross-filesystem: copy + remove */
            fs::copy ( src, dst,
                       fs::copy_options::recursive |
                       fs::copy_options::copy_symlinks, ec );
            if ( ! ec )
            {
                fs::remove_all ( src, ec );
                any_moved = true;
            }
        }
        else
        {
            any_moved = true;
        }
    }

    if ( any_moved )
        sel.clear ( );

    return any_moved;
}

/*
 * sort_label
 *
 * Retorna uma string curta descrevendo o modo de ordenação atual.
 */
static const char * sort_label ( SortMode mode )
{
    switch ( mode )
    {
        case SORT_NAME : return "nome";
        case SORT_SIZE : return "tamanho";
        case SORT_DATE : return "data";
    }
    return "";
}

/*
 * clip_status
 *
 * Retorna uma string de status do clipboard interno para exibição no rodapé.
 * Exibe [yc] e o nome do arquivo quando há conteúdo disponível para pp.
 * Vazio se não houver nada copiado.
 */
static std::string clip_status ( const Clipboard & clip )
{
    if ( clip.mode == CLIP_EMPTY ) return "";
    return "[yc] " + clip.src.filename ( ).string ( );
}

/*
 * render
 *
 * Redesenha toda a interface usando flatten() — O(n) total,
 * eliminando o custo O(n²) do loop original com get(i).
 *
 * Entradas com match de busca ativa são destacadas com A_BOLD.
 * A entrada selecionada usa A_REVERSE.
 * Entradas marcadas para mover exibem prefixo [*D] ou [*F].
 *
 * Parâmetros:
 *   m            referência ao objeto Methods
 *   cursor       índice da entrada selecionada
 *   path         caminho atual
 *   clip         clipboard atual
 *   undo         pilha de undo (para indicar se há undo disponível)
 *   sel          seleção atual para mover
 *   search_name  nome da entrada buscada (vazio = sem busca ativa)
 */
static void render
(
    const Methods     & m,
    int                 cursor,
    const fs::path    & path,
    const Clipboard   & clip,
    const UndoStack   & undo,
    const Selection   & sel,
    const std::string & search_name = ""
)
{
    clear ( );

    int rows, cols;
    getmaxyx ( stdscr, rows, cols );

    const int list_rows = rows - 5; /* linhas disponíveis para a listagem */

    /* — flatten: O(n) em vez de O(n²) — */
    Entry * flat [ FM_MAX_ENTRIES ];
    int n = m.flatten ( ( Entry ** ) flat );

    if ( n == 0 )
    {
        mvprintw ( 0, 0, "(diretório vazio)" );
    }
    else
    {
        /* scroll: garante que o cursor fique sempre visível */
        int scroll = 0;
        if ( cursor >= list_rows )
            scroll = cursor - list_rows + 1;

        int display_end = scroll + list_rows;
        if ( display_end > n ) display_end = n;

        for ( int i = scroll; i < display_end; i ++ )
        {
            int row = i - scroll;

            bool is_cursor   = ( i == cursor );
            bool is_match    = ( ! search_name.empty ( )
                                 && flat [ i ] -> getName ( ) == search_name );
            bool is_selected = sel.has ( flat [ i ] -> getName ( ) );

            if ( is_cursor   ) attron  ( A_REVERSE );
            if ( is_match    ) attron  ( A_BOLD    );
            if ( is_selected ) attron  ( A_BOLD    );

            /* prefixo: [*D]/[*F] se selecionado, [D]/[F] caso contrário */
            const char * prefix;
            if ( is_selected )
                prefix = ( flat [ i ] -> getType ( ) == ENTRY_DIR ) ? "[*D]" : "[*F]";
            else
                prefix = ( flat [ i ] -> getType ( ) == ENTRY_DIR ) ? "[D] " : "[F] ";

            mvprintw ( row, 0, "%s%s", prefix, flat [ i ] -> getName ( ).c_str ( ) );

            if ( is_selected ) attroff ( A_BOLD    );
            if ( is_match    ) attroff ( A_BOLD    );
            if ( is_cursor   ) attroff ( A_REVERSE );
        }
    }

    /* — rodapé — */
    mvprintw ( rows - 5, 0, "%-*s", cols, path.c_str ( ) );

    /* status: clipboard + seleção + undo + ordenação */
    std::string status;
    std::string cs = clip_status ( clip );
    if ( ! cs.empty ( )   ) status += cs + "  ";
    if ( ! sel.empty ( )  ) status += "[sel: " + std::to_string ( sel.size ( ) ) + "]  ";
    if ( ! undo.empty ( ) ) status += "[u: desfazer]  ";
    status += std::string ( "ord: " ) + sort_label ( m.getSortMode ( ) );

    mvprintw ( rows - 4, 0, "%-*s", cols, status.c_str ( ) );

    /* busca ativa */
    if ( ! search_name.empty ( ) )
        mvprintw ( rows - 3, 0, "busca: %s", search_name.c_str ( ) );
    else
        mvprintw ( rows - 3, 0, "%-*s", cols, "" );

    /* mapa de teclas */
    mvprintw
    (
        rows - 2,
        0,
        "Enter-abrir  Bksp-voltar  yc-copiar conteudo  yy-copiar entrada  "
        "pp-colar  dd-deletar  u-desfazer  a-criar arq  A-criar pasta  "
        "r-renomear  /-buscar  s-ordenar  P-copiar path  C-clonar  "
        "Spc-selecionar  M-mover selecao  ESC-limpar sel  q-sair"
    );

    refresh ( );
}

/*
 * cmd_search
 *
 * Solicita um prefixo ao usuário e usa search_prefix da BST
 * para localizar entradas. Retorna o índice in-order do primeiro
 * resultado, ou -1 se não encontrado.
 *
 * A busca tenta primeiro ENTRY_DIR, depois ENTRY_FILE.
 * O nome do primeiro match é devolvido em result_name para
 * que o render possa destacá-lo.
 *
 * Complexidade da busca: O(log n + k)
 *
 * Parâmetros:
 *   m            referência ao Methods
 *   result_name  saída: nome da primeira entrada encontrada
 *
 * Retorno:
 *   índice in-order do primeiro resultado, ou -1 se não encontrado
 */
static int cmd_search ( const Methods & m, std::string & result_name )
{
    std::string prefix = prompt_input ( "/buscar: " );
    result_name.clear ( );

    if ( prefix.empty ( ) )
        return -1;

    Entry * results [ FM_MAX_ENTRIES ];
    int found = m.search_prefix ( prefix, ( Entry ** ) results, FM_MAX_ENTRIES );

    if ( found == 0 )
        return -1;

    result_name = results [ 0 ] -> getName ( );
    return m.find_index ( result_name );
}

/*
 * main
 *
 * Loop principal do navegador de arquivos.
 *
 * Mapa completo de teclas e sequências:
 *
 *   Navegação:
 *     KEY_DOWN         desce o cursor (circular)
 *     KEY_UP           sobe o cursor (circular)
 *     Enter            entra em pasta / abre arquivo no editor
 *     Backspace        sobe um nível no diretório
 *
 *   Clipboard:
 *     yc               copia conteúdo do arquivo → clipboard do sistema + interno (pp)
 *     yy               copia path da entrada → clipboard do sistema apenas
 *     pp               cola conteúdo de yc como novo arquivo no dir atual
 *     dd               move para trash (desfazível com u)
 *
 *   Seleção e mover:
 *     Space            seleciona/deseleciona entrada sob o cursor e avança cursor
 *     M                move todas as entradas selecionadas para destino informado
 *     ESC              limpa a seleção sem mover
 *
 *   Edição (teclas avulsas, chegam direto ao loop):
 *     a                cria novo arquivo (add file)
 *     A                cria nova pasta (Add directory)
 *     r                renomeia entrada selecionada
 *     u                desfaz última deleção (restaura do trash)
 *
 *   Utilitários:
 *     /                busca por prefixo na BST — O(log n + k)
 *     s                cicla modo de ordenação (nome → tamanho → data)
 *     P                copia path do diretório atual para clipboard do sistema
 *     C                clona arquivo/pasta selecionado no mesmo diretório
 *     q                encerra o programa
 */
int main ( int argc, char ** argv )
{
    fs::path   path = fs::current_path ( );
    Methods    m ( FM_MAX_ENTRIES );
    Clipboard  clip;
    KeySeq     seq;
    UndoStack  undo;
    Selection  sel;

    std::string search_name; /* nome do último resultado de busca */

    m.load ( path );

    ncurses_init ( );

    int cursor = 0;
    int key;

    render ( m, cursor, path, clip, undo, sel, search_name );

    while ( true )
    {
        key = getch ( );

        if ( key == ERR )
        {
            render ( m, cursor, path, clip, undo, sel, search_name );
            continue;
        }

        int cmd = seq.push ( key );

        if ( cmd == 0 )
            continue;

        if ( cmd == 'q' )
            break;

        if ( cmd == 'P' )
            copy_current_path ( path );

        const int count = m.count ( );

        if ( cmd == 'C' && count > 0 )
        {
            clone_entry ( path, m.get ( cursor ) );
            m.load ( path );
        }

        /* — navegação — */
        if ( cmd == KEY_DOWN )
        {
            cursor = ( count > 0 ) ? ( cursor + 1 ) % count : 0;
            search_name.clear ( ); /* limpa destaque ao mover */
        }

        if ( cmd == KEY_UP )
        {
            cursor = ( count > 0 ) ? ( cursor - 1 + count ) % count : 0;
            search_name.clear ( );
        }

        /* — entrar em pasta / abrir arquivo — */
        if ( ( cmd == KEY_ENTER || cmd == '\n' ) && count > 0 )
        {
            auto entry = m.get ( cursor );

            if ( entry -> getType ( ) == ENTRY_DIR )
            {
                fs::path next = path / entry -> getName ( );
                if ( fs::exists ( next ) && fs::is_directory ( next ) )
                {
                    path = fs::canonical ( next );
                    m.load ( path );
                    cursor = 0;
                    search_name.clear ( );
                    sel.clear ( ); /* limpa seleção ao trocar de diretório */
                }
            }
            else if ( entry -> getType ( ) == ENTRY_FILE )
            {
                open_editor ( path / entry -> getName ( ) );
                m.load ( path );
            }
        }

        /* — voltar um nível — */
        if ( cmd == KEY_BACKSPACE || cmd == 127 || cmd == 8 )
        {
            path = path.parent_path ( );
            m.load ( path );
            cursor = 0;
            search_name.clear ( );
            sel.clear ( ); /* limpa seleção ao trocar de diretório */
        }

        /* — criar arquivo — */
        if ( cmd == 'a' )
        {
            create_file ( path );
            m.load ( path );
        }

        /* — criar pasta — */
        if ( cmd == 'A' )
        {
            create_dir ( path );
            m.load ( path );
        }

        /* — renomear — */
        if ( cmd == 'r' && count > 0 )
        {
            rename_entry ( path, m.get ( cursor ) );
            m.load ( path );
            search_name.clear ( );
        }

        /* — yc: copiar conteúdo do arquivo — */
        if ( cmd == KeySeq::SEQ_YC && count > 0 )
            cmd_yc ( path, m.get ( cursor ), clip );

        /* — yy: copiar path da entrada para clipboard do sistema — */
        if ( cmd == KeySeq::SEQ_YY && count > 0 )
            cmd_yy ( path, m.get ( cursor ) );

        /* — pp: colar — */
        if ( cmd == KeySeq::SEQ_PP )
        {
            cmd_pp ( path, clip );
            m.load ( path );
        }

        /* — dd: mover para trash — */
        if ( cmd == KeySeq::SEQ_DD && count > 0 )
        {
            if ( cmd_dd ( path, m.get ( cursor ), undo ) )
            {
                m.load ( path );
                if ( cursor >= m.count ( ) && cursor > 0 )
                    cursor --;
                search_name.clear ( );
            }
        }

        /* — u: desfazer última deleção — */
        if ( cmd == 'u' )
        {
            if ( undo.pop_undo ( ) )
            {
                m.load ( path );
                search_name.clear ( );
            }
        }

        /* — /: busca por prefixo na BST — */
        if ( cmd == '/' )
        {
            int idx = cmd_search ( m, search_name );
            if ( idx >= 0 )
                cursor = idx;
            /* se não encontrado, search_name fica vazio e render mostra nada */
        }

        /* — s: ciclar modo de ordenação — */
        if ( cmd == 's' )
        {
            SortMode next;
            switch ( m.getSortMode ( ) )
            {
                case SORT_NAME : next = SORT_SIZE; break;
                case SORT_SIZE : next = SORT_DATE; break;
                case SORT_DATE : next = SORT_NAME; break;
                default        : next = SORT_NAME;
            }
            m.setSortMode ( next, path );
            cursor = 0;
            search_name.clear ( );
        }

        /* — Space: selecionar/deselecionar entrada e avançar cursor — */
        if ( cmd == ' ' && count > 0 )
        {
            sel.toggle ( m.get ( cursor ) -> getName ( ) );
            /* avança cursor automaticamente para agilizar seleção em sequência */
            cursor = ( cursor + 1 ) % count;
        }

        /* — M: mover seleção para destino — */
        if ( cmd == 'M' && ! sel.empty ( ) )
        {
            if ( cmd_move_selection ( path, sel ) )
            {
                m.load ( path );
                if ( cursor >= m.count ( ) && cursor > 0 )
                    cursor --;
                search_name.clear ( );
            }
        }

        /* — ESC: limpar seleção — */
        if ( cmd == 27 )
            sel.clear ( );

        render ( m, cursor, path, clip, undo, sel, search_name );
    }

    endwin ( );
    return 0;
}
        if ( cmd == 27 )
            sel.clear ( );

        render ( m, cursor, path, clip, undo, sel, search_name );
    }

    endwin ( );
    return 0;
}
        if ( cmd == 27 )
            sel.clear ( );

        render ( m, cursor, path, clip, undo, sel, search_name );
    }

    endwin ( );
    return 0;
}