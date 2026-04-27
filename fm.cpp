#include "Methods.hpp"
#include <ncurses.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <poll.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>
#include <set>

/* ============================================================
 *  DirWatcher
 *
 *  Encapsula inotify para monitorar um diretório em tempo real.
 *  Detecta criação, deleção e renomeação de entradas e atualiza
 *  a BST automaticamente sem nenhuma ação do usuário.
 * ============================================================ */
struct DirWatcher
{
    int inotify_fd = -1;
    int watch_fd   = -1;

    static constexpr uint32_t MASK =
        IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
        IN_CLOSE_WRITE | IN_ATTRIB;

    DirWatcher ( ) { inotify_fd = inotify_init1 ( IN_NONBLOCK ); }
    ~ DirWatcher ( ) { stop ( ); }

    void watch ( const fs::path & path )
    {
        if ( inotify_fd < 0 ) return;
        if ( watch_fd >= 0 ) { inotify_rm_watch ( inotify_fd, watch_fd ); watch_fd = -1; }
        drain ( );
        watch_fd = inotify_add_watch ( inotify_fd, path.c_str ( ), MASK );
    }

    bool changed ( )
    {
        if ( inotify_fd < 0 || watch_fd < 0 ) return false;
        struct pollfd pfd { inotify_fd, POLLIN, 0 };
        if ( poll ( & pfd, 1, 0 ) <= 0 ) return false;
        if ( ! ( pfd.revents & POLLIN ) ) return false;
        drain ( );
        return true;
    }

    void stop ( )
    {
        if ( inotify_fd < 0 ) return;
        if ( watch_fd >= 0 ) { inotify_rm_watch ( inotify_fd, watch_fd ); watch_fd = -1; }
        close ( inotify_fd );
        inotify_fd = -1;
    }

    private:
    void drain ( )
    {
        if ( inotify_fd < 0 ) return;
        char buf [ 4096 ] __attribute__ (( aligned ( __alignof__ ( inotify_event ) ) ));
        while ( read ( inotify_fd, buf, sizeof ( buf ) ) > 0 ) { }
    }
};

/* ============================================================ */

enum ClipboardMode { CLIP_EMPTY, CLIP_CONTENT };

struct Clipboard
{
    ClipboardMode  mode    = CLIP_EMPTY;
    std::string    content = "";
    fs::path       src     = "";
};

struct UndoEntry { fs::path trash_path; fs::path origin_path; };

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

    bool push_delete ( const fs::path & origin )
    {
        fs::path tdir = trash_dir ( );
        fs::path dst  = tdir / origin.filename ( );
        int suffix = 0;
        while ( fs::exists ( dst ) )
            dst = tdir / ( origin.filename ( ).string ( ) + "." + std::to_string ( ++ suffix ) );

        std::error_code ec;
        fs::rename ( origin, dst, ec );
        if ( ec )
        {
            fs::copy_options opts = fs::copy_options::recursive | fs::copy_options::copy_symlinks;
            fs::copy ( origin, dst, opts, ec );
            if ( ec ) return false;
            fs::remove_all ( origin, ec );
            if ( ec ) return false;
        }
        stack.push_back ( { dst, origin } );
        return true;
    }

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
            fs::copy_options opts = fs::copy_options::recursive | fs::copy_options::copy_symlinks;
            fs::copy ( top.trash_path, top.origin_path, opts, ec );
            if ( ec ) return false;
            fs::remove_all ( top.trash_path, ec );
        }
        return ! ec;
    }

    bool empty ( ) const { return stack.empty ( ); }
};

struct Selection
{
    std::set < std::string > names;

    void toggle ( const std::string & name )
    {
        if ( names.count ( name ) ) names.erase  ( name );
        else                        names.insert ( name );
    }

    bool has   ( const std::string & name ) const { return names.count ( name ) > 0; }
    void clear ( )                                 { names.clear ( ); }
    bool empty ( )                           const { return names.empty ( ); }
    int  size  ( )                           const { return ( int ) names.size ( ); }
};

struct KeySeq
{
    static constexpr int SEQ_YC     = -1;
    static constexpr int SEQ_DD     = -2;
    static constexpr int SEQ_YY     = -3;
    static constexpr int SEQ_PP     = -4;
    static constexpr int TIMEOUT_MS = 400;

    using Clock = std::chrono::steady_clock;

    int               first   = 0;
    bool              waiting = false;
    Clock::time_point last_time;

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

            if ( ! timed_out && first == 'y' )
            {
                if ( key == 'y' ) return SEQ_YY;
                if ( key == 'c' ) return SEQ_YC;
            }

            return key;
        }

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

/* ============================================================
 *  Funções auxiliares
 * ============================================================ */

static void ncurses_init ( )
{
    initscr ( );
    noecho ( );
    cbreak ( );
    keypad ( stdscr, TRUE );
    curs_set ( 0 );
    timeout ( 100 );
}

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

        if ( ch == 27 ) { input.clear ( ); break; }
        if ( ch == '\n' || ch == KEY_ENTER ) break;
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

static void open_editor ( const fs::path & file )
{
    const char * editor = getenv ( "EDITOR" );
    if ( ! editor ) editor = "vim";

    std::string path = file.string ( );
    def_prog_mode ( );
    endwin ( );

    pid_t pid = fork ( );
    if ( pid == 0 )
    {
        char * args [ ] = { ( char * ) editor, ( char * ) path.c_str ( ), NULL };
        execvp ( editor, args );
        exit ( 1 );
    }
    if ( pid > 0 ) waitpid ( pid, NULL, 0 );

    reset_prog_mode ( );
    refresh ( );
}

static bool create_file ( const fs::path & path )
{
    std::string name = prompt_input ( "Novo arquivo: " );
    if ( name.empty ( ) ) return false;
    fs::path target = path / name;
    if ( fs::exists ( target ) ) return false;
    std::ofstream f ( target );
    return f.good ( );
}

static bool create_dir ( const fs::path & path )
{
    std::string name = prompt_input ( "Nova pasta: " );
    if ( name.empty ( ) ) return false;
    fs::path target = path / name;
    if ( fs::exists ( target ) ) return false;
    return fs::create_directory ( target );
}

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

static void copy_to_system_clipboard ( const std::string & value )
{
    std::string cmd =
        "printf '%s' '" + value + "' | wl-copy 2>/dev/null || "
        "printf '%s' '" + value + "' | xclip -selection clipboard 2>/dev/null";
    system ( cmd.c_str ( ) );
}

static bool cmd_yc ( const fs::path & path, const Entry * entry, Clipboard & clip )
{
    if ( entry -> getType ( ) != ENTRY_FILE ) return false;
    fs::path src = path / entry -> getName ( );
    std::ifstream f ( src );
    if ( ! f.is_open ( ) ) return false;
    std::ostringstream ss;
    ss << f.rdbuf ( );
    clip.content = ss.str ( );
    clip.src     = src;
    clip.mode    = CLIP_CONTENT;
    copy_to_system_clipboard ( clip.content );
    return true;
}

static void cmd_yy ( const fs::path & path, const Entry * entry )
{
    copy_to_system_clipboard ( ( path / entry -> getName ( ) ).string ( ) );
}

static bool cmd_pp ( const fs::path & path, const Clipboard & clip )
{
    if ( clip.mode == CLIP_EMPTY ) return false;
    std::string name = prompt_input ( "Colar como: " );
    if ( name.empty ( ) ) return false;
    fs::path dst = path / name;
    if ( fs::exists ( dst ) ) return false;
    std::ofstream f ( dst );
    if ( ! f.is_open ( ) ) return false;
    f << clip.content;
    return f.good ( );
}

static bool cmd_dd ( const fs::path & path, const Entry * entry, UndoStack & undo )
{
    std::string confirm = prompt_input ( "Deletar \"" + entry -> getName ( ) + "\"? (s/n): " );
    if ( confirm != "s" && confirm != "S" ) return false;
    return undo.push_delete ( path / entry -> getName ( ) );
}

static void copy_current_path ( const fs::path & path )
{
    copy_to_system_clipboard ( path.string ( ) );
}

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

static bool cmd_move_selection ( const fs::path & path, Selection & sel )
{
    if ( sel.empty ( ) ) return false;
    std::string dest_str = prompt_input ( "Mover para: " );
    if ( dest_str.empty ( ) ) return false;

    fs::path dest = dest_str;
    if ( ! dest_str.empty ( ) && dest_str [ 0 ] == '~' )
    {
        const char * home = getenv ( "HOME" );
        if ( home ) dest = fs::path ( home ) / dest_str.substr ( 1 );
    }

    std::error_code ec;
    if ( ! fs::exists ( dest ) ) fs::create_directories ( dest, ec );
    if ( ! fs::is_directory ( dest ) ) return false;

    bool any_moved = false;
    for ( const auto & name : sel.names )
    {
        fs::path src = path / name;
        fs::path dst = dest / name;
        if ( ! fs::exists ( src ) || fs::exists ( dst ) ) continue;

        fs::rename ( src, dst, ec );
        if ( ec )
        {
            fs::copy ( src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec );
            if ( ! ec ) { fs::remove_all ( src, ec ); any_moved = true; }
        }
        else any_moved = true;
    }

    if ( any_moved ) sel.clear ( );
    return any_moved;
}

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

static std::string clip_status ( const Clipboard & clip )
{
    if ( clip.mode == CLIP_EMPTY ) return "";
    return "[yc] " + clip.src.filename ( ).string ( );
}

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
    const int list_rows = rows - 5;

    Entry * flat [ FM_MAX_ENTRIES ];
    int n = m.flatten ( ( Entry ** ) flat );

    if ( n == 0 )
    {
        mvprintw ( 0, 0, "(diretório vazio)" );
    }
    else
    {
        int scroll = 0;
        if ( cursor >= list_rows ) scroll = cursor - list_rows + 1;
        int display_end = scroll + list_rows;
        if ( display_end > n ) display_end = n;

        for ( int i = scroll; i < display_end; i ++ )
        {
            int  row         = i - scroll;
            bool is_cursor   = ( i == cursor );
            bool is_match    = ( ! search_name.empty ( ) && flat [ i ] -> getName ( ) == search_name );
            bool is_selected = sel.has ( flat [ i ] -> getName ( ) );

            if ( is_cursor   ) attron  ( A_REVERSE );
            if ( is_match    ) attron  ( A_BOLD    );
            if ( is_selected ) attron  ( A_BOLD    );

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

    mvprintw ( rows - 5, 0, "%-*s", cols, path.c_str ( ) );

    std::string status;
    std::string cs = clip_status ( clip );
    if ( ! cs.empty ( )   ) status += cs + "  ";
    if ( ! sel.empty ( )  ) status += "[sel: " + std::to_string ( sel.size ( ) ) + "]  ";
    if ( ! undo.empty ( ) ) status += "[u: desfazer]  ";
    status += std::string ( "ord: " ) + sort_label ( m.getSortMode ( ) );

    mvprintw ( rows - 4, 0, "%-*s", cols, status.c_str ( ) );

    if ( ! search_name.empty ( ) )
        mvprintw ( rows - 3, 0, "busca: %s", search_name.c_str ( ) );
    else
        mvprintw ( rows - 3, 0, "%-*s", cols, "" );

    mvprintw ( rows - 2, 0,
        "Enter-abrir  Bksp-voltar  yc-copiar conteudo  yy-copiar entrada  "
        "pp-colar  dd-deletar  u-desfazer  a-criar arq  A-criar pasta  "
        "r-renomear  /-buscar  s-ordenar  P-copiar path  C-clonar  "
        "Spc-selecionar  M-mover selecao  ESC-limpar sel  q-sair" );

    refresh ( );
}

static int cmd_search ( const Methods & m, std::string & result_name )
{
    std::string prefix = prompt_input ( "/buscar: " );
    result_name.clear ( );
    if ( prefix.empty ( ) ) return -1;

    Entry * results [ FM_MAX_ENTRIES ];
    int found = m.search_prefix ( prefix, ( Entry ** ) results, FM_MAX_ENTRIES );
    if ( found == 0 ) return -1;

    result_name = results [ 0 ] -> getName ( );
    return m.find_index ( result_name );
}

/* ============================================================
 *  main
 * ============================================================ */
int main ( int argc, char ** argv )
{
    /* ── --lastdir <arquivo> ──────────────────────────────────
     *
     * O wrapper fm-shell.sh passa este argumento com um arquivo
     * temporário. Ao sair com 'q', fm grava o diretório atual
     * nesse arquivo e o shell executa cd automaticamente.
     * ─────────────────────────────────────────────────────── */
    std::string lastdir_file;
    for ( int i = 1; i < argc - 1; i ++ )
    {
        if ( std::string ( argv [ i ] ) == "--lastdir" )
        {
            lastdir_file = argv [ i + 1 ];
            break;
        }
    }

    fs::path   path = fs::current_path ( );
    Methods    m ( FM_MAX_ENTRIES );
    Clipboard  clip;
    KeySeq     seq;
    UndoStack  undo;
    Selection  sel;
    DirWatcher watcher;

    std::string search_name;

    m.load ( path );
    watcher.watch ( path );

    ncurses_init ( );

    int cursor = 0;
    int key;

    render ( m, cursor, path, clip, undo, sel, search_name );

    while ( true )
    {
        key = getch ( );   /* bloqueia no máximo 100 ms */

        /* ── inotify: reload automático ──────────────────────
         *
         * Após cada getch(), consulta o inotify via poll() sem
         * bloquear. Se o diretório mudou externamente, recarrega
         * a BST e redesenha sem nenhuma ação do usuário.
         * ─────────────────────────────────────────────────── */
        if ( watcher.changed ( ) )
        {
            m.load ( path );
            if ( cursor >= m.count ( ) && cursor > 0 )
                cursor = m.count ( ) - 1;
            search_name.clear ( );
            render ( m, cursor, path, clip, undo, sel, search_name );
        }

        if ( key == ERR )
        {
            render ( m, cursor, path, clip, undo, sel, search_name );
            continue;
        }

        int cmd = seq.push ( key );
        if ( cmd == 0 ) continue;

        if ( cmd == 'q' ) break;

        if ( cmd == 'P' ) copy_current_path ( path );

        const int count = m.count ( );

        if ( cmd == 'C' && count > 0 )
        {
            clone_entry ( path, m.get ( cursor ) );
            m.load ( path );
        }

        if ( cmd == KEY_DOWN )
        {
            cursor = ( count > 0 ) ? ( cursor + 1 ) % count : 0;
            search_name.clear ( );
        }

        if ( cmd == KEY_UP )
        {
            cursor = ( count > 0 ) ? ( cursor - 1 + count ) % count : 0;
            search_name.clear ( );
        }

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
                    watcher.watch ( path );
                    cursor = 0;
                    search_name.clear ( );
                    sel.clear ( );
                }
            }
            else if ( entry -> getType ( ) == ENTRY_FILE )
            {
                open_editor ( path / entry -> getName ( ) );
                m.load ( path );
                watcher.watch ( path );   /* drena eventos do editor */
            }
        }

        if ( cmd == KEY_BACKSPACE || cmd == 127 || cmd == 8 )
        {
            path = path.parent_path ( );
            m.load ( path );
            watcher.watch ( path );
            cursor = 0;
            search_name.clear ( );
            sel.clear ( );
        }

        if ( cmd == 'a' ) { create_file ( path ); m.load ( path ); }
        if ( cmd == 'A' ) { create_dir  ( path ); m.load ( path ); }

        if ( cmd == 'r' && count > 0 )
        {
            rename_entry ( path, m.get ( cursor ) );
            m.load ( path );
            search_name.clear ( );
        }

        if ( cmd == KeySeq::SEQ_YC && count > 0 )
            cmd_yc ( path, m.get ( cursor ), clip );

        if ( cmd == KeySeq::SEQ_YY && count > 0 )
            cmd_yy ( path, m.get ( cursor ) );

        if ( cmd == KeySeq::SEQ_PP )
        {
            cmd_pp ( path, clip );
            m.load ( path );
        }

        if ( cmd == KeySeq::SEQ_DD && count > 0 )
        {
            if ( cmd_dd ( path, m.get ( cursor ), undo ) )
            {
                m.load ( path );
                if ( cursor >= m.count ( ) && cursor > 0 ) cursor --;
                search_name.clear ( );
            }
        }

        if ( cmd == 'u' )
        {
            if ( undo.pop_undo ( ) ) { m.load ( path ); search_name.clear ( ); }
        }

        if ( cmd == '/' )
        {
            int idx = cmd_search ( m, search_name );
            if ( idx >= 0 ) cursor = idx;
        }

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

        if ( cmd == ' ' && count > 0 )
        {
            sel.toggle ( m.get ( cursor ) -> getName ( ) );
            cursor = ( cursor + 1 ) % count;
        }

        if ( cmd == 'M' && ! sel.empty ( ) )
        {
            if ( cmd_move_selection ( path, sel ) )
            {
                m.load ( path );
                if ( cursor >= m.count ( ) && cursor > 0 ) cursor --;
                search_name.clear ( );
            }
        }

        if ( cmd == 27 ) sel.clear ( );

        render ( m, cursor, path, clip, undo, sel, search_name );
    }

    watcher.stop ( );

    /* ── grava diretório atual para o wrapper de shell ──────── */
    if ( ! lastdir_file.empty ( ) )
    {
        std::ofstream lf ( lastdir_file );
        if ( lf.is_open ( ) ) lf << path.string ( ) << "\n";
    }

    endwin ( );
    return 0;
}