#ifndef METHODS_HPP
#define METHODS_HPP

#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <fstream>

#define FM_MAX_ENTRIES 512

using std::string, std::cout, std::endl;
namespace fs = std::filesystem;

typedef enum
{
    ENTRY_DIR,
    ENTRY_FILE
} EntryType;

/*
 * SortMode
 *
 * Define o critério de ordenação usado pela BST em Methods.
 *   SORT_NAME  → dirs antes de arquivos, depois ordem alfabética (padrão)
 *   SORT_SIZE  → dirs antes de arquivos, depois tamanho crescente
 *   SORT_DATE  → dirs antes de arquivos, depois data de modificação decrescente
 */
enum SortMode
{
    SORT_NAME,
    SORT_SIZE,
    SORT_DATE
};

/* ============================================================
 *  Entry
 *
 *  Representa uma entrada do sistema de arquivos (arquivo ou pasta).
 *  Armazena nome, extensão, tipo, tamanho e data de modificação.
 *
 *  A extensão é extraída automaticamente do nome no construtor
 *  e atualizada em setName. Para diretórios sempre fica vazia.
 * ============================================================ */
class Entry
{
    private :
        string    name;
        string    extension;
        EntryType type;
        uintmax_t size;
        fs::file_time_type mtime;

        /*
         * parse
         *
         * Extrai a extensão de um nome de arquivo.
         * Retorna "" se não houver ponto no nome.
         */
        string parse ( const string & filename )
        {
            size_t dot = filename.rfind ( '.' );
            return ( dot != string::npos ) ? filename.substr ( dot ) : "";
        }

    public :
        Entry ( string name, EntryType type,
                uintmax_t sz = 0,
                fs::file_time_type mt = fs::file_time_type { } )
        {
            this -> name      = name;
            this -> type      = type;
            this -> extension = ( type == ENTRY_FILE ) ? parse ( name ) : "";
            this -> size      = sz;
            this -> mtime     = mt;
        }

        string             getName      ( ) const { return name;      }
        string             getExtension ( ) const { return extension; }
        EntryType          getType      ( ) const { return type;      }
        uintmax_t          getSize      ( ) const { return size;      }
        fs::file_time_type getMtime     ( ) const { return mtime;     }

        void setName ( string new_name )
        {
            name      = new_name;
            extension = ( type == ENTRY_FILE ) ? parse ( new_name ) : "";
        }
};

/* ============================================================
 *  EntryNode
 *
 *  Nó da árvore binária de busca (BST) de entradas.
 * ============================================================ */
struct EntryNode
{
    Entry     * data;
    EntryNode * left;
    EntryNode * right;

    EntryNode ( Entry * e )
        : data ( e ), left ( nullptr ), right ( nullptr ) { }
};

/* ============================================================
 *  Methods
 *
 *  Gerencia as entradas de um diretório usando uma BST interna.
 *  Suporta três modos de ordenação configuráveis via setSortMode.
 * ============================================================ */
class Methods
{
    private :
        EntryNode * root;
        int         _count;
        int         capacity;
        SortMode    sort_mode;

        /* --------------------------------------------------------
         *  compare_keys
         *
         *  Define a ordem da BST de acordo com sort_mode:
         *    SORT_NAME → dirs antes de arquivos, depois nome alfab.
         *    SORT_SIZE → dirs antes de arquivos, depois tamanho asc.
         *    SORT_DATE → dirs antes de arquivos, depois data desc.
         *
         *  Em todos os modos o nome é usado como desempate final
         *  para garantir unicidade na BST.
         * -------------------------------------------------------- */
        int compare_keys ( const Entry * a, const Entry * b ) const
        {
            bool a_dir = ( a -> getType ( ) == ENTRY_DIR );
            bool b_dir = ( b -> getType ( ) == ENTRY_DIR );

            /* dirs sempre primeiro */
            if ( a_dir != b_dir )
                return b_dir ? 1 : -1;

            if ( sort_mode == SORT_SIZE && ! a_dir )
            {
                if ( a -> getSize ( ) < b -> getSize ( ) ) return -1;
                if ( a -> getSize ( ) > b -> getSize ( ) ) return  1;
            }
            else if ( sort_mode == SORT_DATE )
            {
                /* mais recente primeiro → ordem inversa de mtime */
                if ( a -> getMtime ( ) > b -> getMtime ( ) ) return -1;
                if ( a -> getMtime ( ) < b -> getMtime ( ) ) return  1;
            }

            /* desempate sempre pelo nome */
            if ( a -> getName ( ) < b -> getName ( ) ) return -1;
            if ( a -> getName ( ) > b -> getName ( ) ) return  1;
            return 0;
        }

        /* --------------------------------------------------------
         *  insert_node
         * -------------------------------------------------------- */
        EntryNode * insert_node ( EntryNode * root_node, EntryNode * node )
        {
            if ( root_node == nullptr )
                return node;

            int cmp = compare_keys ( node -> data, root_node -> data );

            if ( cmp < 0 )
                root_node -> left  = insert_node ( root_node -> left,  node );
            else if ( cmp > 0 )
                root_node -> right = insert_node ( root_node -> right, node );

            return root_node;
        }

        /* --------------------------------------------------------
         *  find_min
         * -------------------------------------------------------- */
        EntryNode * find_min ( EntryNode * node ) const
        {
            while ( node -> left != nullptr )
                node = node -> left;
            return node;
        }

        /* --------------------------------------------------------
         *  remove_node
         * -------------------------------------------------------- */
        EntryNode * remove_node ( EntryNode * root_node, const string & name, EntryType type )
        {
            if ( root_node == nullptr )
                return nullptr;

            Entry probe ( name, type );
            int cmp = compare_keys ( & probe, root_node -> data );

            if ( cmp < 0 )
            {
                root_node -> left = remove_node ( root_node -> left, name, type );
            }
            else if ( cmp > 0 )
            {
                root_node -> right = remove_node ( root_node -> right, name, type );
            }
            else
            {
                if ( root_node -> left == nullptr )
                {
                    EntryNode * right_child = root_node -> right;
                    delete root_node -> data;
                    delete root_node;
                    _count --;
                    return right_child;
                }

                if ( root_node -> right == nullptr )
                {
                    EntryNode * left_child = root_node -> left;
                    delete root_node -> data;
                    delete root_node;
                    _count --;
                    return left_child;
                }

                EntryNode * successor = find_min ( root_node -> right );
                delete root_node -> data;
                root_node -> data  = new Entry ( successor -> data -> getName ( ),
                                                 successor -> data -> getType ( ),
                                                 successor -> data -> getSize ( ),
                                                 successor -> data -> getMtime ( ) );
                root_node -> right = remove_node ( root_node -> right,
                                                   successor -> data -> getName ( ),
                                                   successor -> data -> getType ( ) );
            }

            return root_node;
        }

        /* --------------------------------------------------------
         *  search_node
         *
         *  Busca binária exata por nome + tipo na BST.
         *  Complexidade: O(log n) médio
         * -------------------------------------------------------- */
        Entry * search_node ( EntryNode * root_node, const string & name, EntryType type ) const
        {
            if ( root_node == nullptr )
                return nullptr;

            Entry probe ( name, type );
            int cmp = compare_keys ( & probe, root_node -> data );

            if ( cmp == 0 ) return root_node -> data;
            if ( cmp <  0 ) return search_node ( root_node -> left,  name, type );
            return search_node ( root_node -> right, name, type );
        }

        /* --------------------------------------------------------
         *  search_prefix_node
         *
         *  Busca por prefixo de nome na BST.
         *  Retorna o índice in-order do primeiro resultado encontrado
         *  e preenche out com todos os matches.
         *
         *  Complexidade: O(log n + k) onde k = resultados.
         * -------------------------------------------------------- */
        void search_prefix_node
        (
            EntryNode         * root_node,
            const string      & prefix,
            EntryType           type,
            Entry            ** out,
            int               & out_count
        ) const
        {
            if ( root_node == nullptr ) return;

            const string & name    = root_node -> data -> getName ( );
            EntryType      current = root_node -> data -> getType ( );

            bool same_type = ( current == type );

            if ( same_type )
            {
                bool matches = ( name.size ( ) >= prefix.size ( ) )
                    && ( name.compare ( 0, prefix.size ( ), prefix ) == 0 );

                if ( name >= prefix )
                    search_prefix_node ( root_node -> left, prefix, type, out, out_count );

                if ( matches )
                    out [ out_count ++ ] = root_node -> data;

                if ( name.compare ( 0, prefix.size ( ), prefix ) <= 0
                     || name < prefix )
                    search_prefix_node ( root_node -> right, prefix, type, out, out_count );
                else if ( matches )
                    search_prefix_node ( root_node -> right, prefix, type, out, out_count );
            }
            else
            {
                search_prefix_node ( root_node -> left,  prefix, type, out, out_count );
                search_prefix_node ( root_node -> right, prefix, type, out, out_count );
            }
        }

        /* --------------------------------------------------------
         *  find_index_of
         *
         *  Retorna o índice in-order de uma Entry pelo nome.
         *  Usado após busca por prefixo para posicionar o cursor.
         *  Retorna -1 se não encontrado.
         * -------------------------------------------------------- */
        int find_index_of ( EntryNode * node, const string & name, int & current ) const
        {
            if ( node == nullptr ) return -1;

            int left_result = find_index_of ( node -> left, name, current );
            if ( left_result != -1 ) return left_result;

            if ( node -> data -> getName ( ) == name )
            {
                int idx = current;
                current ++;
                return idx;
            }
            current ++;

            return find_index_of ( node -> right, name, current );
        }

        /* --------------------------------------------------------
         *  inorder
         * -------------------------------------------------------- */
        void inorder ( EntryNode * node, Entry ** out, int & out_index ) const
        {
            if ( node == nullptr ) return;
            inorder ( node -> left, out, out_index );
            out [ out_index ++ ] = node -> data;
            inorder ( node -> right, out, out_index );
        }

        /* --------------------------------------------------------
         *  destroy
         * -------------------------------------------------------- */
        void destroy ( EntryNode * node )
        {
            if ( node == nullptr ) return;
            destroy ( node -> left  );
            destroy ( node -> right );
            delete node -> data;
            delete node;
        }

        /* --------------------------------------------------------
         *  get_at
         * -------------------------------------------------------- */
        Entry * get_at ( EntryNode * node, int target, int & current ) const
        {
            if ( node == nullptr ) return nullptr;

            Entry * left_result = get_at ( node -> left, target, current );
            if ( left_result ) return left_result;

            if ( current == target ) { current ++; return node -> data; }
            current ++;

            return get_at ( node -> right, target, current );
        }

    public :

        Methods ( int capacity = FM_MAX_ENTRIES )
            : root ( nullptr ), _count ( 0 ), capacity ( capacity ),
              sort_mode ( SORT_NAME ) { }

        ~ Methods ( )
        {
            clear ( );
        }

        int      count    ( ) const { return _count;    }
        SortMode getSortMode ( ) const { return sort_mode; }

        /* --------------------------------------------------------
         *  setSortMode
         *
         *  Muda o critério de ordenação e reconstrói a BST
         *  mantendo as entradas já carregadas.
         * -------------------------------------------------------- */
        void setSortMode ( SortMode mode, const fs::path & current_path )
        {
            if ( sort_mode == mode ) return;
            sort_mode = mode;
            load ( current_path ); /* reconstrói com novo critério */
        }

        /* --------------------------------------------------------
         *  get
         * -------------------------------------------------------- */
        Entry * get ( int i ) const
        {
            int current = 0;
            return get_at ( root, i, current );
        }

        /* --------------------------------------------------------
         *  push
         *
         *  Insere uma nova entrada lendo metadados do disco.
         * -------------------------------------------------------- */
        void push ( const fs::path & full_path, const string & name, EntryType type )
        {
            if ( _count >= capacity ) return;

            uintmax_t          sz  = 0;
            fs::file_time_type mt  = { };

            std::error_code ec;
            if ( type == ENTRY_FILE )
                sz = fs::file_size ( full_path, ec );

            mt = fs::last_write_time ( full_path, ec );

            EntryNode * node = new EntryNode ( new Entry ( name, type, sz, mt ) );
            root = insert_node ( root, node );
            _count ++;
        }

        /* --------------------------------------------------------
         *  remove
         * -------------------------------------------------------- */
        void remove ( const string & name, EntryType type )
        {
            root = remove_node ( root, name, type );
        }

        /* --------------------------------------------------------
         *  search
         *
         *  Busca binária exata por nome + tipo.
         *  Complexidade: O(log n) médio
         * -------------------------------------------------------- */
        Entry * search ( const string & name, EntryType type ) const
        {
            return search_node ( root, name, type );
        }

        /* --------------------------------------------------------
         *  search_prefix
         *
         *  Busca todas as entradas cujo nome começa com prefix.
         *  Busca primeiro em ENTRY_DIR, depois em ENTRY_FILE.
         *  Preenche out e retorna quantidade encontrada.
         *
         *  Complexidade: O(log n + k)
         * -------------------------------------------------------- */
        int search_prefix ( const string & prefix, Entry ** out, int max_out ) const
        {
            int out_count = 0;
            /* busca dirs primeiro, depois arquivos — mesma ordem do render */
            search_prefix_node ( root, prefix, ENTRY_DIR,  out, out_count );
            search_prefix_node ( root, prefix, ENTRY_FILE, out, out_count );
            ( void ) max_out;
            return out_count;
        }

        /* --------------------------------------------------------
         *  find_index
         *
         *  Retorna o índice in-order de uma Entry pelo nome.
         *  Retorna -1 se não encontrado.
         * -------------------------------------------------------- */
        int find_index ( const string & name ) const
        {
            int current = 0;
            return find_index_of ( root, name, current );
        }

        /* --------------------------------------------------------
         *  flatten
         *
         *  Preenche out com todas as entradas em ordem in-order.
         *  Complexidade: O(n)
         * -------------------------------------------------------- */
        int flatten ( Entry ** out ) const
        {
            int idx = 0;
            inorder ( root, out, idx );
            return idx;
        }

        /* --------------------------------------------------------
         *  clear
         * -------------------------------------------------------- */
        void clear ( )
        {
            destroy ( root );
            root   = nullptr;
            _count = 0;
        }

        /* --------------------------------------------------------
         *  load
         *
         *  Lê o diretório do disco e insere cada entrada na BST
         *  com os metadados necessários para a ordenação ativa.
         * -------------------------------------------------------- */
        void load ( const fs::path & path )
        {
            clear ( );

            if ( ! fs::exists ( path ) || ! fs::is_directory ( path ) )
                return;

            for ( const auto & entry : fs::directory_iterator ( path ) )
            {
                EntryType t    = entry.is_directory ( ) ? ENTRY_DIR : ENTRY_FILE;
                string    name = entry.path ( ).filename ( ).string ( );
                push ( entry.path ( ), name, t );
            }
        }

        /* --------------------------------------------------------
         *  print
         * -------------------------------------------------------- */
        void print ( ) const
        {
            if ( _count == 0 )
            {
                cout << "arvore vazia\n";
                return;
            }

            Entry * flat [ FM_MAX_ENTRIES ];
            int n = flatten ( ( Entry ** ) flat );

            for ( int i = 0; i < n; i ++ )
            {
                const char * prefix =
                    ( flat [ i ] -> getType ( ) == ENTRY_DIR ) ? "[D]" : "[F]";
                cout << prefix << " " << flat [ i ] -> getName ( ) << "\n";
            }
        }

        /* --------------------------------------------------------
         *  rename_by_extension
         * -------------------------------------------------------- */
        void rename_by_extension ( const fs::path & folder )
        {
            Entry * flat [ FM_MAX_ENTRIES ];
            int n = flatten ( ( Entry ** ) flat );

            for ( int i = 0; i < n; i ++ )
            {
                if ( flat [ i ] -> getType ( ) != ENTRY_FILE ) continue;

                string filename  = flat [ i ] -> getName      ( );
                string extension = flat [ i ] -> getExtension ( );

                if ( extension.empty ( ) ) continue;

                string prefix   = extension.substr ( 1 );
                string new_name = prefix + "_" + filename;

                if ( filename.find ( prefix + "_" ) == 0 ) continue;

                try
                {
                    fs::rename ( folder / filename, folder / new_name );
                }
                catch ( const fs::filesystem_error & e )
                {
                    cout << "error: " << e.what ( ) << "\n";
                }
            }

            load ( folder );
        }
};

#endif