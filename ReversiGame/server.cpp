#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <thread>
#include <arpa/inet.h>
#include <sqlite3.h>
#include <vector>
#include <queue>
#include <mutex>

#define PORT 8080
#define BUFFER_SIZE 1024

enum States
{
    WAITING_FOR_PLAYER,
    IN_GAME,
    FREE
};

typedef struct
{
    int socket;
    int logged_in;
    char username[50];
    int game_id;
    States status;
} Client_Info;

typedef struct
{
    Client_Info *player1;
    Client_Info *player2;
    int board[8][8];
    int turn;
} Game_Info;

sqlite3 *db;
std::vector<Game_Info> active_games;
std::queue<Client_Info *> waiting_queue;
std::mutex waiting_mutex;
std::mutex games_mutex;

void send_message_to_client(int socket, char *message)
{
    send(socket, message, strlen(message), 0);
}

void init_database()
{
    if (sqlite3_open("users.db", &db) != SQLITE_OK)
    {
        fprintf(stderr, "Nu am putut deschide baza de date: %s\n", sqlite3_errmsg(db));
        exit(EXIT_FAILURE);
    }

    const char *create_table = "CREATE TABLE IF NOT EXISTS users("
                               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               "username TEXT UNIQUE NOT NULL,"
                               "password TEXT NOT NULL,"
                               "logged_in INTEGER DEFAULT 0,"
                               "score INTEGER DEFAULT 0);";
    char *err_msg = NULL;
    if (sqlite3_exec(db, create_table, NULL, NULL, &err_msg) != SQLITE_OK)
    {
        fprintf(stderr, "Eroare la crearea tabelei: %s\n", err_msg);
        sqlite3_free(err_msg);
        exit(EXIT_FAILURE);
    }
}

void register_user(const char *username, const char *password, int socket)
{
    char response[BUFFER_SIZE];
    bzero(response, BUFFER_SIZE);
    const char *sql = "INSERT INTO users (username,password) VALUES (?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "Eroare la statement: %s\n", sqlite3_errmsg(db));
        return;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    int result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (result == SQLITE_DONE)
    {
        snprintf(response, BUFFER_SIZE, "Inregistrare reusita pentru utilizatorul %s.\n", username);
        send_message_to_client(socket, response);
    }
    else
    {
        snprintf(response, BUFFER_SIZE, "Utilizatorul %s deja exista.\n", username);
        send_message_to_client(socket, response);
    }
}

int login_user(const char *username, const char *password)
{
    const char *check_sql = "SELECT logged_in FROM users WHERE username=? AND password=?;";
    const char *update_sql = "UPDATE users SET logged_in=1 WHERE username=?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "Eroare la statement:%s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    int result = sqlite3_step(stmt);
    if (result == SQLITE_ROW)
    {
        int is_logged_in = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        if (is_logged_in)
        {
            fprintf(stderr, "Utilizatorul %s este deja logat.\n", username);
            return 2;
        }
    }
    else
    {
        sqlite3_finalize(stmt);
        fprintf(stderr, "Credentiale invalide.\n");
        return 0;
    }

    if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "Eroare la statement:%s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return result == SQLITE_DONE;
}

void logout_user(const char *username)
{
    const char *sql = "UPDATE users SET logged_in = 0 WHERE username=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "Eroare la statement: %s\n", sqlite3_errmsg(db));
        return;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void scoreboard(Client_Info *client_info)
{
    const char *sql = "SELECT username, score FROM users ORDER BY score DESC LIMIT 10;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
        std::string scoreboard = "Top 10 Players:\n";
        int rank = 1;

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const char *username = (const char *)sqlite3_column_text(stmt, 0);
            int score = sqlite3_column_int(stmt, 1);
            scoreboard += std::to_string(rank) + ". " + username + ": " + std::to_string(score) + " points\n";
            rank++;
        }

        sqlite3_finalize(stmt);
        send_message_to_client(client_info->socket, (char *)scoreboard.c_str());
    }
}

bool is_valid_move(int board[8][8], int row, int col, int player)
{
    if (board[row][col] != 0)
        return false;

    int opponent = (player == 1) ? 2 : 1;
    bool valid = false;

    int dirs[8][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};

    for (int dir = 0; dir < 8; dir++)
    {
        int curr_row = row + dirs[dir][0];
        int curr_col = col + dirs[dir][1];
        bool found_opponent = false;

        while (curr_row >= 0 && curr_row < 8 && curr_col >= 0 && curr_col < 8)
        {
            if (board[curr_row][curr_col] == opponent)
            {
                found_opponent = true;
            }
            else if (board[curr_row][curr_col] == player && found_opponent)
            {
                valid = true;
                break;
            }
            else
            {
                break;
            }
            curr_row += dirs[dir][0];
            curr_col += dirs[dir][1];
        }
        if (valid)
            break;
    }
    return valid;
}

void make_move(int board[8][8], int row, int col, int player)
{
    board[row][col] = player;
    int opponent = (player == 1) ? 2 : 1;

    int dirs[8][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};

    for (int dir = 0; dir < 8; dir++)
    {
        int curr_row = row + dirs[dir][0];
        int curr_col = col + dirs[dir][1];
        std::vector<std::pair<int, int>> to_flip;

        while (curr_row >= 0 && curr_row < 8 && curr_col >= 0 && curr_col < 8)
        {
            if (board[curr_row][curr_col] == opponent)
            {
                to_flip.push_back({curr_row, curr_col});
            }
            else if (board[curr_row][curr_col] == player && !to_flip.empty())
            {
                for (auto &pos : to_flip)
                {
                    board[pos.first][pos.second] = player;
                }
                break;
            }
            else
            {
                break;
            }
            curr_row += dirs[dir][0];
            curr_col += dirs[dir][1];
        }
    }
}

void init_board(int board[8][8])
{
    memset(board, 0, sizeof(int) * 64);
    board[3][3] = 2;
    board[3][4] = 1;
    board[4][3] = 1;
    board[4][4] = 2;
}

bool has_valid_moves(int board[8][8], int player)
{
    for (int i = 0; i < 8; i++)
    {
        for (int j = 0; j < 8; j++)
        {
            if (is_valid_move(board, i, j, player))
            {
                return true;
            }
        }
    }
    return false;
}

void update_score(const char *username, int points)
{
    const char *sql = "UPDATE users SET score = score + ? WHERE username = ?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "Eroare la statement: %s\n", sqlite3_errmsg(db));
        return;
    }

    sqlite3_bind_int(stmt, 1, points);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        fprintf(stderr, "Eroare la actualizarea scorurilor: %s\n", sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
}

std::string get_board_string(int board[8][8])
{
    std::string result = "Tabla curenta:\n";
    result += "  0 1 2 3 4 5 6 7\n";

    for (int i = 0; i < 8; i++)
    {
        result += std::to_string(i) + " ";
        for (int j = 0; j < 8; j++)
        {
            if (board[i][j] == 0)
                result += ". ";
            else if (board[i][j] == 1)
                result += "B ";
            else
                result += "W ";
        }
        result += "\n";
    }
    return result;
}

void handle_move(Client_Info *client_info, char *move_str)
{
    char response[BUFFER_SIZE];
    bzero(response, BUFFER_SIZE);

    Game_Info &game = active_games[client_info->game_id];

    bool is_player1 = (game.player1 == client_info);
    if ((is_player1 && game.turn != 1) || (!is_player1 && game.turn != 2))
    {
        std::string board_str = get_board_string(game.board);
        snprintf(response, BUFFER_SIZE, "Nu este randul tau!\n%s", board_str.c_str());
        send_message_to_client(client_info->socket, response);
        return;
    }

    int row, col;
    if (sscanf(move_str, "%d %d", &row, &col) != 2 ||
        row < 0 || row >= 8 || col < 0 || col >= 8)
    {
        snprintf(response, BUFFER_SIZE, "Format invalid! move <linie> <coloana>\n");
        send_message_to_client(client_info->socket, response);
        return;
    }

    if (!is_valid_move(game.board, row, col, game.turn))
    {
        std::string board_str = get_board_string(game.board);
        snprintf(response, BUFFER_SIZE, "Miscare invalida!Mai incearca.\n%s", board_str.c_str());
        send_message_to_client(client_info->socket, response);
        return;
    }

    make_move(game.board, row, col, game.turn);

    game.turn = (game.turn == 1) ? 2 : 1;

    if (!has_valid_moves(game.board, game.turn))
    {
        game.turn = (game.turn == 1) ? 2 : 1;
        if (!has_valid_moves(game.board, game.turn))
        {
            int black_count = 0, white_count = 0;
            for (int i = 0; i < 8; i++)
            {
                for (int j = 0; j < 8; j++)
                {
                    if (game.board[i][j] == 1)
                        black_count++;
                    else if (game.board[i][j] == 2)
                        white_count++;
                }
            }

            if (black_count > white_count)
            {
                update_score(game.player1->username, 3);
                update_score(game.player2->username, 1);
            }
            else if (white_count > black_count)
            {
                update_score(game.player1->username, 1);
                update_score(game.player2->username, 3);
            }
            else
            {
                update_score(game.player1->username, 2);
                update_score(game.player2->username, 2);
            }

            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Game Over!\nNegru: %d\nAlb: %d\n%s",
                     black_count, white_count, board_str.c_str());
            game.player1->status = FREE;
            game.player2->status = FREE;
            send_message_to_client(game.player1->socket, response);
            send_message_to_client(game.player2->socket, response);

            game.player1->game_id = -1;
            game.player2->game_id = -1;
            return;
        }
    }

    std::string board_str = get_board_string(game.board);
    snprintf(response, BUFFER_SIZE, "Mutare corecta! Muta %s\n%s",
             (game.turn == 1) ? game.player1->username : game.player2->username,
             board_str.c_str());
    send_message_to_client(game.player1->socket, response);
    send_message_to_client(game.player2->socket, response);
}

void create_new_game(Client_Info *player1, Client_Info *player2)
{
    Game_Info new_game;
    new_game.player1 = player1;
    new_game.player2 = player2;
    init_board(new_game.board);
    new_game.turn = 1;

    std::lock_guard<std::mutex> lock(games_mutex);
    active_games.push_back(new_game);

    player1->game_id = active_games.size() - 1;
    player2->game_id = active_games.size() - 1;

    char response[BUFFER_SIZE];
    std::string board_str = get_board_string(new_game.board);

    snprintf(response, BUFFER_SIZE, "Jocul a inceput! Tu esti cu piesele negre(black)(B). %s\n", board_str.c_str());
    send_message_to_client(player1->socket, response);

    snprintf(response, BUFFER_SIZE, "Jocul a inceput! Tu esti cu piesele albe(white)(W) %s\n", board_str.c_str());
    send_message_to_client(player2->socket, response);

    printf("Joc creat: %s (Black) vs %s (White)\n", player1->username, player2->username);
}

void handle_play(Client_Info *client_info)
{
    char response[BUFFER_SIZE];
    bzero(response, BUFFER_SIZE);
    std::lock_guard<std::mutex> lock(waiting_mutex);
    if (!waiting_queue.empty())
    {
        Client_Info *player1 = waiting_queue.front();
        waiting_queue.pop();
        client_info->status = IN_GAME;
        player1->status = IN_GAME;
        create_new_game(player1, client_info);
    }
    else
    {
        waiting_queue.push(client_info);
        snprintf(response, BUFFER_SIZE, "Asteptati un adversar!\n");
        client_info->status = WAITING_FOR_PLAYER;
        send_message_to_client(client_info->socket, response);
    }
}

void handle_command(Client_Info *client_info, char *command)
{
    char response[BUFFER_SIZE];
    if (strncmp(command, "register", 8) == 0)
    {
        bzero(response, BUFFER_SIZE);
        char *credentials = command + 8;
        char *username = strtok(credentials, " ");
        char *password = strtok(NULL, " ");
        if (client_info->status == WAITING_FOR_PLAYER)
        {
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n");
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->status == IN_GAME)
        {
            Game_Info &game = active_games[client_info->game_id];
            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aeasta comanda decat dupa ce termini meciul!\n%s", board_str.c_str());
            send_message_to_client(client_info->socket, response);
        }
        else if (username && password)
        {
            register_user(username, password, client_info->socket);
        }
        else
        {
            snprintf(response, BUFFER_SIZE, "Sintaxa: register <username> <password>\n");
            send_message_to_client(client_info->socket, response);
        }
    }
    else if (strncmp(command, "login", 5) == 0)
    {
        bzero(response, BUFFER_SIZE);
        char *credentials = command + 5;
        char *username = strtok(credentials, " ");
        char *password = strtok(NULL, " ");
        if (client_info->status == WAITING_FOR_PLAYER)
        {
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n");
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->status == IN_GAME)
        {
            Game_Info &game = active_games[client_info->game_id];
            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n%s", board_str.c_str());
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->logged_in == 1)
        {
            snprintf(response, BUFFER_SIZE, "Esti deja logat cu un alt cont!\n");
            send_message_to_client(client_info->socket, response);
        }
        else if (username && password)
        {
            if (login_user(username, password) == 1)
            {
                printf("praici");
                client_info->logged_in = 1;
                strncpy(client_info->username, username, sizeof(client_info->username));
                snprintf(response, BUFFER_SIZE, "Login reusit!\n");
                send_message_to_client(client_info->socket, response);
            }
            else if (login_user(username, password) == 2)
            {
                snprintf(response, BUFFER_SIZE, "Esti deja conectat!\n");
                send_message_to_client(client_info->socket, response);
            }
            else
            {
                snprintf(response, BUFFER_SIZE, "Login esuat!Verificati credentialele!\n");
                send_message_to_client(client_info->socket, response);
            }
        }
        else
        {
            snprintf(response, BUFFER_SIZE, "Sintaxa:login <username> <password>\n");
            send_message_to_client(client_info->socket, response);
        }
    }
    else if (strcmp(command, "logout") == 0)
    {
        bzero(response, BUFFER_SIZE);
        if (client_info->status == WAITING_FOR_PLAYER)
        {
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n");
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->status == IN_GAME)
        {
            Game_Info &game = active_games[client_info->game_id];
            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n%s", board_str.c_str());
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->logged_in)
        {
            client_info->logged_in = 0;
            logout_user(client_info->username);
            bzero(client_info->username, sizeof(client_info->username));
            snprintf(response, BUFFER_SIZE, "Logout reusit!\n");
            send_message_to_client(client_info->socket, response);
        }
        else
        {
            snprintf(response, BUFFER_SIZE, "Nu esti logat!\n");
            send_message_to_client(client_info->socket, response);
        }
    }
    else if (strcmp(command, "play") == 0)
    {
        bzero(response, BUFFER_SIZE);
        if (client_info->status == WAITING_FOR_PLAYER)
        {
            snprintf(response, BUFFER_SIZE, "Cauti deja un meci!\n");
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->status == IN_GAME)
        {
            Game_Info &game = active_games[client_info->game_id];
            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n%s", board_str.c_str());
            send_message_to_client(client_info->socket, response);
        }
        else if (!client_info->logged_in)
        {
            snprintf(response, BUFFER_SIZE, "Trebuie sa fii logat pentru a te juca!\n");
            send_message_to_client(client_info->socket, response);
        }
        else
        {
            handle_play(client_info);
        }
    }
    else if (strncmp(command, "move", 4) == 0)
    {
        bzero(response, BUFFER_SIZE);
        if (!client_info->logged_in)
        {
            snprintf(response, BUFFER_SIZE, "Trebuie sa fii logat pentru a executa o mutare!\n");
            send_message_to_client(client_info->socket, response);
            return;
        }
        else if (client_info->game_id == -1 || client_info->status == WAITING_FOR_PLAYER)
        {
            snprintf(response, BUFFER_SIZE, "Nu esti intr-un joc activ!\n");
            send_message_to_client(client_info->socket, response);
            return;
        }
        handle_move(client_info, command + 5);
    }
    else if (strcmp(command, "scoreboard") == 0)
    {
        bzero(response, BUFFER_SIZE);
        if (client_info->status == WAITING_FOR_PLAYER)
        {
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n");
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->status == IN_GAME)
        {
            Game_Info &game = active_games[client_info->game_id];
            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n%s", board_str.c_str());
            send_message_to_client(client_info->socket, response);
        }
        else
        {
            scoreboard(client_info);
        }
    }
    else if (strcmp(command, "surrender") == 0)
    {
        bzero(response, BUFFER_SIZE);
        Game_Info &game = active_games[client_info->game_id];
        if (client_info->game_id == -1 || client_info->status == WAITING_FOR_PLAYER)
        {
            snprintf(response, BUFFER_SIZE, "Nu esti intr-un joc activ!\n");
            send_message_to_client(client_info->socket, response);
            return;
        }
        snprintf(response, BUFFER_SIZE, "%s a abandonat jocul!", client_info->username);
        send_message_to_client(game.player1->socket, response);
        send_message_to_client(game.player2->socket, response);
        if (client_info->username == game.player1->username)
        {
            update_score(game.player1->username, 1);
            update_score(game.player2->username, 3);
        }
        else
        {
            update_score(game.player1->username, 3);
            update_score(game.player2->username, 1);
        }
        game.player1->status = FREE;
        game.player2->status = FREE;

        game.player1->game_id = -1;
        game.player2->game_id = -1;
    }
    else if (strcmp(command, "stop") == 0)
    {
        bzero(response, BUFFER_SIZE);
        std::lock_guard<std::mutex> lock(waiting_mutex);
        if (client_info->status == WAITING_FOR_PLAYER)
        {
            waiting_queue.pop();
            snprintf(response, BUFFER_SIZE, "Am oprit cautarea!\n");
            client_info->status = FREE;
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->status == IN_GAME)
        {
            Game_Info &game = active_games[client_info->game_id];
            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Poti folosi comanda doar daca cauti un meci!\n%s", board_str.c_str());
            send_message_to_client(client_info->socket, response);
        }
        else
        {
            snprintf(response, BUFFER_SIZE, "Poti folosi comanda doar daca cauti un meci!\n");
            send_message_to_client(client_info->socket, response);
        }
    }
    else if (strcmp(command, "help") == 0)
    {
        bzero(response, BUFFER_SIZE);
        const char *help_msg =
            "Comenzi valabile:\n"
            "register <username> <password> - Creaza un nou cont\n"
            "login <username> <password> - Autentificate\n"
            "logout - Log-out din contul curent\n"
            "play - Pregateste un joc Reversi\n"
            "stop - Opreste cautarea unui meci\n"
            "move <linie> <coloana> - Executa o mutare in joc\n"
            "surrender - Abandoneaza jocul curent\n"
            "scoreboard - Top 10 jucatori\n"
            "help - Arata acest mesaj\n"
            "quit - Deconeteaza clientul de la server\n";
        bzero(response, BUFFER_SIZE);
        if (client_info->status == WAITING_FOR_PLAYER)
        {
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n");
            send_message_to_client(client_info->socket, response);
        }
        else if (client_info->status == IN_GAME)
        {
            Game_Info &game = active_games[client_info->game_id];
            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Nu poti utiliza aceasta comanda decat dupa ce termini meciul!\n%s", board_str.c_str());
            send_message_to_client(client_info->socket, response);
        }
        else
        {
            snprintf(response, BUFFER_SIZE, "%s", help_msg);
            send_message_to_client(client_info->socket, response);
        }
    }
    else
    {
        bzero(response, BUFFER_SIZE);
        if (client_info->status == IN_GAME)
        {
            Game_Info &game = active_games[client_info->game_id];
            std::string board_str = get_board_string(game.board);
            snprintf(response, BUFFER_SIZE, "Comanda necunoscuta!\n%s", board_str.c_str());
            send_message_to_client(client_info->socket, response);
        }
        snprintf(response, BUFFER_SIZE, "Comanda necunoscuta");
        send_message_to_client(client_info->socket, response);
    }
}

void *handle_client(void *arg)
{
    Client_Info *client_info = (Client_Info *)arg;
    char buffer[BUFFER_SIZE];

    printf("Clientul %d s-a conectat. \n", client_info->socket);

    while (1)
    {
        bzero(buffer, BUFFER_SIZE);
        int bytes_received = read(client_info->socket, buffer, BUFFER_SIZE - 1);
        char response[BUFFER_SIZE];
        bzero(response, BUFFER_SIZE);
        if (bytes_received <= 0)
        {
            printf("Clientul %d s-a deconectat.\n", client_info->socket);

            if (client_info->logged_in)
            {
                if (client_info->status == WAITING_FOR_PLAYER)
                {
                    waiting_queue.pop();
                    logout_user(client_info->username);
                }
                else if (client_info->status == IN_GAME)
                {
                    Game_Info &game = active_games[client_info->game_id];
                    snprintf(response, BUFFER_SIZE, "%s a abandonat jocul!", client_info->username);

                    if (client_info->username == game.player1->username)
                    {
                        update_score(game.player1->username, 1);
                        update_score(game.player2->username, 3);
                        send_message_to_client(game.player2->socket, response);
                        game.player2->status = FREE;
                        game.player2->game_id = -1;
                    }
                    else
                    {
                        update_score(game.player1->username, 3);
                        update_score(game.player2->username, 1);
                        send_message_to_client(game.player1->socket, response);
                        game.player1->status = FREE;
                        game.player1->game_id = -1;
                    }
                    logout_user(client_info->username);
                }
                else
                {
                    logout_user(client_info->username);
                }
            }

            close(client_info->socket);
            free(client_info);
            return NULL;
        }

        buffer[strcspn(buffer, "\n")] = 0;

        printf("Comandă primită: %s [from client %d]\n", buffer, client_info->socket);

        handle_command(client_info, buffer);
    }
}

int main()
{
    init_database();
    int server_socket;
    struct sockaddr_in server_address, client_address;
    socklen_t client_addr_len = sizeof(client_address);
    int optval = 1;

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == 0)
    {
        perror("Eroare la crearea socket-ului");
        exit(EXIT_FAILURE);
    }

    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("Eroare la bind");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 5) < 0)
    {
        perror("Eroare la listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Serverul ascultă pe portul %d...\n", PORT);

    while (1)
    {

        int client_socket = accept(server_socket, (struct sockaddr *)&client_address, &client_addr_len);
        if (client_socket < 0)
        {
            perror("Eroare la accept");
            continue;
        }

        Client_Info *client_info = (Client_Info *)malloc(sizeof(Client_Info));
        client_info->socket = (int)client_socket;
        client_info->logged_in = 0;
        client_info->game_id = -1;
        client_info->status = FREE;
        bzero(client_info->username, sizeof(client_info->username));

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_info) != 0)
        {
            perror("Eroare la crearea thread-ului");
            close(client_socket);
            free(client_info);
        }
        else
        {
            pthread_detach(thread_id);
        }
    }

    sqlite3_close(db);
    close(server_socket);
    return 0;
}
