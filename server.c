/*
 * server.c - Servidor de chat da Padaria
 * ---------------------------------------
 * Serve o arquivo chat.html e styles.css, e expoe duas rotas de API:
 *   GET  /api/mensagens   -> devolve o conteudo do arquivo compartilhado (chat.txt)
 *   POST /api/enviar      -> recebe "nome" e "mensagem" e acrescenta uma linha no chat.txt
 *
 * O arquivo chat.txt e o "bloco de notas" compartilhado: ele deve estar numa
 * pasta compartilhada da rede (ex: um drive mapeado \\PC-DA-COZINHA\chat\chat.txt).
 * Cada pessoa roda esse mesmo server.c na sua propria maquina, todos apontando
 * para o MESMO chat.txt. Assim, quando alguem envia uma mensagem, o arquivo
 * compartilhado muda, e o servidor de cada um le a mudanca e mostra na tela.
 *
 * COMO CONFIGURAR:
 *   1) Edite a constante CHAT_FILE_PATH la embaixo para apontar pro caminho
 *      real do chat.txt na pasta compartilhada da rede.
 *   2) Coloque chat.html e styles.css na MESMA pasta onde esta o server.exe.
 *
 * COMO COMPILAR NO WINDOWS (com MinGW/gcc):
 *   gcc server.c -o server.exe -lws2_32
 *
 * COMO RODAR:
 *   server.exe
 *   Depois abra no navegador: http://localhost:8080/chat.html
 *   (outras pessoas na mesma rede tambem podem acessar pelo IP da sua maquina,
 *    ex: http://192.168.0.10:8080/chat.html -- mas o mais simples e cada um
 *    rodar o proprio server.exe apontando pro chat.txt compartilhado)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* ---------- Caminho do arquivo compartilhado (edite aqui) ---------- */
#define CHAT_FILE_PATH   "chat.txt"
/* Exemplo de caminho de rede no Windows:
   #define CHAT_FILE_PATH   "Z:\\chat\\chat.txt"
   ou
   #define CHAT_FILE_PATH   "\\\\PC-DA-COZINHA\\compartilhado\\chat.txt"
*/

/* Arquivo compartilhado onde ficam as receitas cadastradas pelos usuarios.
   Deve estar na MESMA pasta compartilhada de rede que o CHAT_FILE_PATH,
   assim, quando alguem cadastra uma receita nova, todo mundo que abrir
   o index.html ve ela aparecer. */
#define RECEITAS_FILE_PATH   "receitas.txt"

/* Arquivo compartilhado com as contas cadastradas (nome, email, cpf,
   telefone e a senha ja "embaralhada" por hash_senha - nunca em texto puro).
   Tambem deve ficar na pasta compartilhada de rede, assim a mesma conta
   funciona pra logar em qualquer computador que rode esse server.c. */
#define USUARIOS_FILE_PATH   "usuarios.txt"

#define PORTA            9090
#define TAM_BUFFER       131072

/* ---------- Camada de rede: Windows (Winsock) x Linux/Mac (POSIX) ---------- */
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
    #define FECHAR_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    typedef int socket_t;
    #define FECHAR_SOCKET close
#endif

/* ---------- Utilidades ---------- */

/* Decodifica texto vindo de um formulario (application/x-www-form-urlencoded) */
static void url_decode(const char *entrada, char *saida, size_t tam_saida) {
    size_t i = 0, o = 0;
    while (entrada[i] != '\0' && o + 1 < tam_saida) {
        if (entrada[i] == '+') {
            saida[o++] = ' ';
            i++;
        } else if (entrada[i] == '%' && entrada[i+1] && entrada[i+2]) {
            char hex[3] = { entrada[i+1], entrada[i+2], '\0' };
            saida[o++] = (char) strtol(hex, NULL, 16);
            i += 3;
        } else {
            saida[o++] = entrada[i++];
        }
    }
    saida[o] = '\0';
}

/* Pega o valor de um campo dentro de um corpo tipo "nome=Ana&mensagem=Oi" */
static void pegar_campo(const char *corpo, const char *campo, char *destino, size_t tam_destino) {
    destino[0] = '\0';
    char busca[64];
    snprintf(busca, sizeof(busca), "%s=", campo);
    const char *inicio = strstr(corpo, busca);
    if (!inicio) return;
    inicio += strlen(busca);
    const char *fim = strchr(inicio, '&');
    size_t tam = fim ? (size_t)(fim - inicio) : strlen(inicio);
    if (tam >= tam_destino) tam = tam_destino - 1;

    char bruto[16384];
    if (tam >= sizeof(bruto)) tam = sizeof(bruto) - 1;
    memcpy(bruto, inicio, tam);
    bruto[tam] = '\0';

    url_decode(bruto, destino, tam_destino);
}

/* Le um arquivo inteiro para dentro de um buffer alocado (o chamador deve dar free) */
static char *ler_arquivo(const char *caminho, long *tamanho_lido) {
    FILE *f = fopen(caminho, "rb");
    if (!f) {
        *tamanho_lido = 0;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long tam = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(tam + 1);
    if (!buf) { fclose(f); *tamanho_lido = 0; return NULL; }
    size_t lido = fread(buf, 1, tam, f);
    buf[lido] = '\0';
    fclose(f);
    *tamanho_lido = (long) lido;
    return buf;
}

/* Acrescenta uma linha "Nome: mensagem" no arquivo compartilhado chat.txt */
static void gravar_mensagem(const char *nome, const char *mensagem) {
    FILE *f = fopen(CHAT_FILE_PATH, "a");
    if (!f) {
        printf("[ERRO] Nao consegui abrir '%s' para escrita. Motivo: %s\n", CHAT_FILE_PATH, strerror(errno));
        printf("       (Dica: mova a pasta pra fora do OneDrive/Desktop, ou rode o servidor como administrador)\n");
        return;
    }
    if (strlen(nome) == 0) nome = "Anonimo";
    fprintf(f, "%s: %s\n", nome, mensagem);
    fclose(f);
    printf("[OK] Mensagem gravada em '%s': %s: %s\n", CHAT_FILE_PATH, nome, mensagem);
}

/* (declaradas mais abaixo no arquivo, mas usadas aqui em apagar_receita) */
static int comparar_ignorando_maiusculas(const char *a, const char *b);
static int extrair_campo(const char *linha, int indice, char *destino, size_t tam_destino);

/* Gera um id curto e (na pratica) unico para identificar cada receita,
   misturando o horario atual com um numero aleatorio. */
static void gerar_id_receita(char *saida, size_t tam_saida) {
    snprintf(saida, tam_saida, "%ld%03d", (long) time(NULL), rand() % 1000);
}

/* Acrescenta uma receita nova no arquivo compartilhado receitas.txt.
 * Cada receita ocupa UMA linha no arquivo, com os campos separados por "|||":
 *   id|||nome|||foto|||ingrediente1;;ingrediente2;;ingrediente3|||passo1;;passo2;;passo3|||email_dono
 * (o "foto" pode vir vazio, e os itens de ingredientes/passos vem separados
 *  por ";;", ja prontos assim pelo JavaScript antes de enviar o formulario.
 *  O "email_dono" e o email de quem criou a receita - so essa pessoa pode
 *  apagar a receita depois, veja apagar_receita() mais abaixo) */
static void gravar_receita(const char *id, const char *nome, const char *foto,
                            const char *ingredientes, const char *preparo, const char *email_dono) {
    FILE *f = fopen(RECEITAS_FILE_PATH, "a");
    if (!f) {
        printf("[ERRO] Nao consegui abrir '%s' para escrita. Motivo: %s\n", RECEITAS_FILE_PATH, strerror(errno));
        return;
    }
    if (strlen(nome) == 0) nome = "Receita sem nome";
    fprintf(f, "%s|||%s|||%s|||%s|||%s|||%s\n", id, nome, foto, ingredientes, preparo, email_dono);
    fclose(f);
    printf("[OK] Receita gravada em '%s': %s (dono: %s)\n", RECEITAS_FILE_PATH, nome, email_dono);
}

/* Apaga a receita com o id indicado do receitas.txt, MAS so se o email
 * informado bater com o email do dono que criou a receita (campo 5 da linha).
 * Devolve:
 *   2 -> apagada com sucesso
 *   1 -> a receita existe, mas o email nao e do dono (nao apaga)
 *   0 -> nao achou nenhuma receita com esse id
 */
static int apagar_receita(const char *id, const char *email) {
    long tam = 0;
    char *conteudo = ler_arquivo(RECEITAS_FILE_PATH, &tam);
    if (!conteudo) return 0;

    int resultado = 0;
    char novo_conteudo[TAM_BUFFER];
    novo_conteudo[0] = '\0';
    size_t usado = 0;

    const char *pos = conteudo;
    while (*pos) {
        const char *fim_linha = strchr(pos, '\n');
        size_t tam_linha = fim_linha ? (size_t)(fim_linha - pos) : strlen(pos);
        char linha[8192];
        if (tam_linha >= sizeof(linha)) tam_linha = sizeof(linha) - 1;
        memcpy(linha, pos, tam_linha);
        linha[tam_linha] = '\0';

        int manter_linha = 1;

        if (strlen(linha) > 0) {
            char campo_id[64], campo_email[256];
            extrair_campo(linha, 0, campo_id, sizeof(campo_id));
            extrair_campo(linha, 5, campo_email, sizeof(campo_email));

            if (strcmp(campo_id, id) == 0) {
                if (comparar_ignorando_maiusculas(campo_email, email)) {
                    resultado = 2;      /* apagada */
                    manter_linha = 0;   /* nao copia essa linha pro novo conteudo */
                } else {
                    resultado = 1;      /* existe, mas nao e o dono */
                }
            }
        } else {
            manter_linha = 0; /* nao recopia linhas vazias */
        }

        if (manter_linha && tam_linha > 0) {
            if (usado + tam_linha + 1 < sizeof(novo_conteudo)) {
                memcpy(novo_conteudo + usado, linha, tam_linha);
                usado += tam_linha;
                novo_conteudo[usado++] = '\n';
                novo_conteudo[usado] = '\0';
            }
        }

        pos = fim_linha ? fim_linha + 1 : pos + strlen(pos);
    }

    free(conteudo);

    if (resultado == 2) {
        FILE *f = fopen(RECEITAS_FILE_PATH, "w");
        if (!f) {
            printf("[ERRO] Nao consegui reescrever '%s'. Motivo: %s\n", RECEITAS_FILE_PATH, strerror(errno));
            return 0;
        }
        fwrite(novo_conteudo, 1, usado, f);
        fclose(f);
        printf("[OK] Receita id=%s apagada por %s\n", id, email);
    }

    return resultado;
}

/* ============================================================
 *  LOGIN / CADASTRO DE USUARIOS
 * ============================================================ */

/* Compara duas strings letra por letra ignorando maiusculas/minusculas.
   (nao usamos strcasecmp porque ela nao existe em todo compilador Windows) */
static int comparar_ignorando_maiusculas(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* Copia so os digitos de uma string (tira pontos, tracos, espacos, etc.) */
static int cpf_somente_digitos(const char *cpf, char *saida, size_t tam_saida) {
    size_t o = 0;
    for (size_t i = 0; cpf[i] != '\0' && o + 1 < tam_saida; i++) {
        if (cpf[i] >= '0' && cpf[i] <= '9') saida[o++] = cpf[i];
    }
    saida[o] = '\0';
    return (int) o;
}

/* Validacao de verdade de CPF: confere os dois digitos verificadores
   pelo algoritmo oficial (modulo 11), alem de rejeitar sequencias
   obvias tipo 111.111.111-11. Aceita o CPF com ou sem pontuacao. */
static int cpf_valido(const char *cpf_bruto) {
    char d[16];
    int tam = cpf_somente_digitos(cpf_bruto, d, sizeof(d));
    if (tam != 11) return 0;

    int todos_iguais = 1;
    for (int i = 1; i < 11; i++) {
        if (d[i] != d[0]) { todos_iguais = 0; break; }
    }
    if (todos_iguais) return 0;

    for (int parte = 0; parte < 2; parte++) {
        int tam_calc = 9 + parte;
        int soma = 0, peso = tam_calc + 1;
        for (int i = 0; i < tam_calc; i++) {
            soma += (d[i] - '0') * peso;
            peso--;
        }
        int resto = (soma * 10) % 11;
        if (resto == 10) resto = 0;
        if (resto != (d[tam_calc] - '0')) return 0;
    }
    return 1;
}

/* Transforma a senha digitada num hash hexadecimal (algoritmo FNV-1a).
   Isso evita guardar a senha em texto puro no usuarios.txt. Importante:
   e um hash simples, bom o suficiente pra esse projeto de rede local,
   mas NAO tem o nivel de seguranca de um sistema em producao de verdade. */
static void hash_senha(const char *senha, char *saida_hex, size_t tam_saida_hex) {
    unsigned long long hash = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *) senha; *p; p++) {
        hash ^= (unsigned long long) (*p);
        hash *= 1099511628211ULL;
    }
    snprintf(saida_hex, tam_saida_hex, "%016llx", hash);
}

/* Extrai o campo de indice 'indice' (comecando em 0) de uma linha cujos
   campos sao separados pela sequencia "|||". Ex: extrair_campo(linha, 1, ...)
   pega o segundo campo (o email, no caso do usuarios.txt). */
static int extrair_campo(const char *linha, int indice, char *destino, size_t tam_destino) {
    const char *inicio = linha;
    for (int i = 0; i < indice; i++) {
        const char *sep = strstr(inicio, "|||");
        if (!sep) { destino[0] = '\0'; return 0; }
        inicio = sep + 3;
    }
    const char *fim = strstr(inicio, "|||");
    size_t tam = fim ? (size_t)(fim - inicio) : strlen(inicio);
    if (tam >= tam_destino) tam = tam_destino - 1;
    memcpy(destino, inicio, tam);
    destino[tam] = '\0';

    /* Se o ultimo campo da linha (sem separador "|||" depois dele) veio de um
       arquivo salvo com quebra de linha estilo Windows (\r\n), sobra um '\r'
       grudado no final do valor. Sem isso, comparacoes de email/id nunca
       batem para o ultimo campo da linha (ex: "quem e o dono" da receita). */
    while (tam > 0 && (destino[tam - 1] == '\r' || destino[tam - 1] == '\n')) {
        tam--;
        destino[tam] = '\0';
    }
    return 1;
}

/* Acrescenta uma conta nova no arquivo compartilhado usuarios.txt.
   Formato de cada linha: nome|||email|||cpf(so digitos)|||telefone|||hash_da_senha */
static void gravar_usuario(const char *nome, const char *email, const char *cpf_limpo,
                            const char *telefone, const char *senha_hash) {
    FILE *f = fopen(USUARIOS_FILE_PATH, "a");
    if (!f) {
        printf("[ERRO] Nao consegui abrir '%s' para escrita. Motivo: %s\n", USUARIOS_FILE_PATH, strerror(errno));
        return;
    }
    fprintf(f, "%s|||%s|||%s|||%s|||%s\n", nome, email, cpf_limpo, telefone, senha_hash);
    fclose(f);
    printf("[OK] Conta criada em '%s': %s (%s)\n", USUARIOS_FILE_PATH, nome, email);
}

static int email_ja_cadastrado(const char *email) {
    long tam = 0;
    char *conteudo = ler_arquivo(USUARIOS_FILE_PATH, &tam);
    if (!conteudo) return 0;

    int achou = 0;
    const char *pos = conteudo;
    while (*pos && !achou) {
        const char *fim_linha = strchr(pos, '\n');
        size_t tam_linha = fim_linha ? (size_t)(fim_linha - pos) : strlen(pos);
        char linha[1024];
        if (tam_linha >= sizeof(linha)) tam_linha = sizeof(linha) - 1;
        memcpy(linha, pos, tam_linha);
        linha[tam_linha] = '\0';

        if (strlen(linha) > 0) {
            char campo_email[256];
            extrair_campo(linha, 1, campo_email, sizeof(campo_email));
            if (comparar_ignorando_maiusculas(campo_email, email)) achou = 1;
        }
        pos = fim_linha ? fim_linha + 1 : pos + strlen(pos);
    }

    free(conteudo);
    return achou;
}

static int cpf_ja_cadastrado(const char *cpf_limpo) {
    long tam = 0;
    char *conteudo = ler_arquivo(USUARIOS_FILE_PATH, &tam);
    if (!conteudo) return 0;

    int achou = 0;
    const char *pos = conteudo;
    while (*pos && !achou) {
        const char *fim_linha = strchr(pos, '\n');
        size_t tam_linha = fim_linha ? (size_t)(fim_linha - pos) : strlen(pos);
        char linha[1024];
        if (tam_linha >= sizeof(linha)) tam_linha = sizeof(linha) - 1;
        memcpy(linha, pos, tam_linha);
        linha[tam_linha] = '\0';

        if (strlen(linha) > 0) {
            char campo_cpf[32];
            extrair_campo(linha, 2, campo_cpf, sizeof(campo_cpf));
            if (strcmp(campo_cpf, cpf_limpo) == 0) achou = 1;
        }
        pos = fim_linha ? fim_linha + 1 : pos + strlen(pos);
    }

    free(conteudo);
    return achou;
}

/* Confere email + senha contra o usuarios.txt. Se bater, preenche
   nome_saida com o nome da conta e devolve 1. Caso contrario devolve 0. */
static int login_usuario(const char *email, const char *senha, char *nome_saida, size_t tam_nome_saida) {
    long tam = 0;
    char *conteudo = ler_arquivo(USUARIOS_FILE_PATH, &tam);
    if (!conteudo) return 0;

    char hash_digitado[32];
    hash_senha(senha, hash_digitado, sizeof(hash_digitado));

    int ok = 0;
    const char *pos = conteudo;
    while (*pos && !ok) {
        const char *fim_linha = strchr(pos, '\n');
        size_t tam_linha = fim_linha ? (size_t)(fim_linha - pos) : strlen(pos);
        char linha[1024];
        if (tam_linha >= sizeof(linha)) tam_linha = sizeof(linha) - 1;
        memcpy(linha, pos, tam_linha);
        linha[tam_linha] = '\0';

        if (strlen(linha) > 0) {
            char campo_nome[128], campo_email[256], campo_hash[64];
            extrair_campo(linha, 0, campo_nome, sizeof(campo_nome));
            extrair_campo(linha, 1, campo_email, sizeof(campo_email));
            extrair_campo(linha, 4, campo_hash, sizeof(campo_hash));

            if (comparar_ignorando_maiusculas(campo_email, email) &&
                strcmp(campo_hash, hash_digitado) == 0) {
                snprintf(nome_saida, tam_nome_saida, "%s", campo_nome);
                ok = 1;
            }
        }
        pos = fim_linha ? fim_linha + 1 : pos + strlen(pos);
    }

    free(conteudo);
    return ok;
}

static void enviar_resposta(socket_t cliente, const char *status, const char *tipo_conteudo, const char *corpo, long tam_corpo) {
    char cabecalho[512];
    int n = snprintf(cabecalho, sizeof(cabecalho),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %ld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n", status, tipo_conteudo, tam_corpo);
    send(cliente, cabecalho, n, 0);
    if (tam_corpo > 0) send(cliente, corpo, tam_corpo, 0);
}

static void servir_arquivo_estatico(socket_t cliente, const char *caminho_disco, const char *tipo_conteudo) {
    long tam = 0;
    char *conteudo = ler_arquivo(caminho_disco, &tam);
    if (!conteudo) {
        const char *msg = "Arquivo nao encontrado no servidor.";
        enviar_resposta(cliente, "404 Not Found", "text/plain", msg, (long) strlen(msg));
        return;
    }
    enviar_resposta(cliente, "200 OK", tipo_conteudo, conteudo, tam);
    free(conteudo);
}

/* Descobre o Content-Type olhando a extensao do arquivo pedido */
static const char *tipo_por_extensao(const char *caminho) {
    const char *ponto = strrchr(caminho, '.');
    if (!ponto) return "application/octet-stream";

    if (strcmp(ponto, ".html") == 0) return "text/html";
    if (strcmp(ponto, ".css") == 0)  return "text/css";
    if (strcmp(ponto, ".js") == 0)   return "application/javascript";
    if (strcmp(ponto, ".txt") == 0)  return "text/plain";
    if (strcmp(ponto, ".png") == 0)  return "image/png";
    if (strcmp(ponto, ".jpg") == 0 || strcmp(ponto, ".jpeg") == 0 || strcmp(ponto, ".jfif") == 0) return "image/jpeg";
    if (strcmp(ponto, ".gif") == 0)  return "image/gif";
    if (strcmp(ponto, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(ponto, ".ico") == 0)  return "image/x-icon";

    return "application/octet-stream";
}

/* ---------- Tratamento de cada conexao ---------- */
static void tratar_cliente(socket_t cliente) {
    char requisicao[TAM_BUFFER];
    int recebido = recv(cliente, requisicao, sizeof(requisicao) - 1, 0);
    if (recebido <= 0) return;
    requisicao[recebido] = '\0';

    char metodo[8] = {0}, caminho[256] = {0};
    sscanf(requisicao, "%7s %255s", metodo, caminho);

    if (strcmp(metodo, "GET") == 0 && strcmp(caminho, "/api/mensagens") == 0) {
        long tam = 0;
        char *conteudo = ler_arquivo(CHAT_FILE_PATH, &tam);
        if (!conteudo) {
            enviar_resposta(cliente, "200 OK", "text/plain", "", 0);
        } else {
            enviar_resposta(cliente, "200 OK", "text/plain", conteudo, tam);
            free(conteudo);
        }

    } else if (strcmp(metodo, "GET") == 0 && strcmp(caminho, "/api/receitas") == 0) {
        long tam = 0;
        char *conteudo = ler_arquivo(RECEITAS_FILE_PATH, &tam);
        if (!conteudo) {
            enviar_resposta(cliente, "200 OK", "text/plain", "", 0);
        } else {
            enviar_resposta(cliente, "200 OK", "text/plain", conteudo, tam);
            free(conteudo);
        }

    } else if (strcmp(metodo, "GET") == 0) {
        /* Qualquer outro GET: serve o arquivo pedido direto da pasta.
           Ex: /index.html, /bonito.css, /pao-de-queijo.jfif, /chat.html ... */
        const char *nome_arquivo = (caminho[0] == '/') ? caminho + 1 : caminho;
        if (strlen(nome_arquivo) == 0) nome_arquivo = "index.html"; /* pagina principal */

        servir_arquivo_estatico(cliente, nome_arquivo, tipo_por_extensao(nome_arquivo));

    } else if (strcmp(metodo, "POST") == 0 && strcmp(caminho, "/api/enviar") == 0) {
        /* O corpo do POST vem depois da linha em branco "\r\n\r\n" */
        char *corpo = strstr(requisicao, "\r\n\r\n");
        corpo = corpo ? corpo + 4 : "";

        char nome[128], mensagem[1024];
        pegar_campo(corpo, "nome", nome, sizeof(nome));
        pegar_campo(corpo, "mensagem", mensagem, sizeof(mensagem));

        printf("[RECEBIDO] nome='%s' mensagem='%s'\n", nome, mensagem);

        if (strlen(mensagem) > 0) {
            gravar_mensagem(nome, mensagem);
        } else {
            printf("[AVISO] O campo mensagem chegou vazio, nada foi gravado.\n");
        }
        enviar_resposta(cliente, "200 OK", "text/plain", "ok", 2);

    } else if (strcmp(metodo, "POST") == 0 && strcmp(caminho, "/api/nova-receita") == 0) {
        char *corpo = strstr(requisicao, "\r\n\r\n");
        corpo = corpo ? corpo + 4 : "";

        char nome[128], foto[256], ingredientes[4096], preparo[8192], email[256];
        pegar_campo(corpo, "nome", nome, sizeof(nome));
        pegar_campo(corpo, "foto", foto, sizeof(foto));
        pegar_campo(corpo, "ingredientes", ingredientes, sizeof(ingredientes));
        pegar_campo(corpo, "preparo", preparo, sizeof(preparo));
        pegar_campo(corpo, "email", email, sizeof(email));

        printf("[RECEBIDO] nova receita: nome='%s' dono='%s'\n", nome, email);

        if (strlen(nome) > 0) {
            char id[64];
            gerar_id_receita(id, sizeof(id));
            gravar_receita(id, nome, foto, ingredientes, preparo, email);
        } else {
            printf("[AVISO] O campo nome chegou vazio, nenhuma receita foi gravada.\n");
        }
        enviar_resposta(cliente, "200 OK", "text/plain", "ok", 2);

    } else if (strcmp(metodo, "POST") == 0 && strcmp(caminho, "/api/apagar-receita") == 0) {
        char *corpo = strstr(requisicao, "\r\n\r\n");
        corpo = corpo ? corpo + 4 : "";

        char id[64], email[256];
        pegar_campo(corpo, "id", id, sizeof(id));
        pegar_campo(corpo, "email", email, sizeof(email));

        printf("[RECEBIDO] apagar receita: id='%s' email='%s'\n", id, email);

        if (strlen(id) == 0 || strlen(email) == 0) {
            const char *msg = "ERRO|||Dados incompletos";
            enviar_resposta(cliente, "200 OK", "text/plain", msg, (long) strlen(msg));
        } else {
            int resultado = apagar_receita(id, email);
            if (resultado == 2) {
                enviar_resposta(cliente, "200 OK", "text/plain", "OK|||Receita apagada", 21);
            } else if (resultado == 1) {
                const char *msg = "ERRO|||Somente quem criou a receita pode apaga-la";
                enviar_resposta(cliente, "200 OK", "text/plain", msg, (long) strlen(msg));
            } else {
                const char *msg = "ERRO|||Receita nao encontrada";
                enviar_resposta(cliente, "200 OK", "text/plain", msg, (long) strlen(msg));
            }
        }

    } else if (strcmp(metodo, "POST") == 0 && strcmp(caminho, "/api/cadastrar") == 0) {
        char *corpo = strstr(requisicao, "\r\n\r\n");
        corpo = corpo ? corpo + 4 : "";

        char nome[128], email[256], cpf[32], telefone[32], senha[128];
        pegar_campo(corpo, "nome", nome, sizeof(nome));
        pegar_campo(corpo, "email", email, sizeof(email));
        pegar_campo(corpo, "cpf", cpf, sizeof(cpf));
        pegar_campo(corpo, "telefone", telefone, sizeof(telefone));
        pegar_campo(corpo, "senha", senha, sizeof(senha));

        printf("[RECEBIDO] cadastro: nome='%s' email='%s'\n", nome, email);

        if (strlen(nome) == 0 || strlen(email) == 0 || strlen(cpf) == 0 ||
            strlen(telefone) == 0 || strlen(senha) == 0) {
            enviar_resposta(cliente, "200 OK", "text/plain", "ERRO|||Preencha todos os campos", 32);
        } else if (!cpf_valido(cpf)) {
            const char *msg = "ERRO|||CPF invalido";
            enviar_resposta(cliente, "200 OK", "text/plain", msg, (long) strlen(msg));
        } else if (email_ja_cadastrado(email)) {
            const char *msg = "ERRO|||Ja existe uma conta com esse email";
            enviar_resposta(cliente, "200 OK", "text/plain", msg, (long) strlen(msg));
        } else {
            char cpf_limpo[16];
            cpf_somente_digitos(cpf, cpf_limpo, sizeof(cpf_limpo));
            if (cpf_ja_cadastrado(cpf_limpo)) {
                const char *msg = "ERRO|||Ja existe uma conta com esse CPF";
                enviar_resposta(cliente, "200 OK", "text/plain", msg, (long) strlen(msg));
            } else {
                char senha_hash[32];
                hash_senha(senha, senha_hash, sizeof(senha_hash));
                gravar_usuario(nome, email, cpf_limpo, telefone, senha_hash);

                char resposta[256];
                int n = snprintf(resposta, sizeof(resposta), "OK|||%s", nome);
                enviar_resposta(cliente, "200 OK", "text/plain", resposta, n);
            }
        }

    } else if (strcmp(metodo, "POST") == 0 && strcmp(caminho, "/api/login") == 0) {
        char *corpo = strstr(requisicao, "\r\n\r\n");
        corpo = corpo ? corpo + 4 : "";

        char email[256], senha[128], nome[128];
        pegar_campo(corpo, "email", email, sizeof(email));
        pegar_campo(corpo, "senha", senha, sizeof(senha));

        printf("[RECEBIDO] login: email='%s'\n", email);

        if (login_usuario(email, senha, nome, sizeof(nome))) {
            char resposta[256];
            int n = snprintf(resposta, sizeof(resposta), "OK|||%s", nome);
            enviar_resposta(cliente, "200 OK", "text/plain", resposta, n);
        } else {
            const char *msg = "ERRO|||Email ou senha invalidos";
            enviar_resposta(cliente, "200 OK", "text/plain", msg, (long) strlen(msg));
        }

    } else {
        const char *msg = "Rota nao encontrada.";
        enviar_resposta(cliente, "404 Not Found", "text/plain", msg, (long) strlen(msg));
    }
}

int main(void) {
    srand((unsigned int) time(NULL));

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    socket_t servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor < 0) {
        printf("Erro ao criar o socket.\n");
        return 1;
    }

    int opcao = 1;
    setsockopt(servidor, SOL_SOCKET, SO_REUSEADDR, (const char *) &opcao, sizeof(opcao));

    struct sockaddr_in endereco;
    memset(&endereco, 0, sizeof(endereco));
    endereco.sin_family = AF_INET;
    endereco.sin_addr.s_addr = INADDR_ANY;
    endereco.sin_port = htons(PORTA);

    if (bind(servidor, (struct sockaddr *) &endereco, sizeof(endereco)) < 0) {
        printf("Erro ao dar bind na porta %d. Ela ja esta em uso?\n", PORTA);
        return 1;
    }

    listen(servidor, 10);
    printf("Servidor do chat rodando em http://localhost:%d/chat.html\n", PORTA);
    printf("Arquivo compartilhado sendo usado: %s\n", CHAT_FILE_PATH);

    while (1) {
        struct sockaddr_in endereco_cliente;
        socklen_t tam_endereco = sizeof(endereco_cliente);
        socket_t cliente = accept(servidor, (struct sockaddr *) &endereco_cliente, &tam_endereco);
        if (cliente < 0) continue;

        tratar_cliente(cliente);
        FECHAR_SOCKET(cliente);
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}