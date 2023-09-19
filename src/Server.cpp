#include "ParserUtils.hpp"
#include "Server.hpp"
#include "ServerConfig.hpp"
#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include "ServerUtils.hpp"
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/types.h>
#include <sys/wait.h>

#define VALIDPATH 0
#define AUTOINDEX 1
#define INVALIDPATH 2


// BEGIN: Helper Functions Prototypes
RouteCfg*       find_route(const HttpRequest& req, std::vector<RouteCfg>& routes);
HttpResponse    list_dir_res(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route);
HttpResponse    response_cgi(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route);
HttpResponse    response_get(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route);
HttpResponse    response_head(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route);
HttpResponse    response_delete(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route);
HttpResponse    response_error(const HttpRequest& req, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route, const int statusCode);
// END: Helper Functions Prototypes


// BEGIN: Canonical Form Functions
Server::Server(const ServerCfg& cfg) :_cfg(cfg) {}

Server::Server(const Server& copy) :_cfg(copy._cfg) {}

Server::~Server() {}

Server& Server::operator=(const Server& copy)
{
    _cfg = copy._cfg;
    return *this;
}
// END: Canonical Form Functions


// BEGIN: Class Functions
// Will take a request and handle it, which includes calling CGI
HttpResponse    Server::handleRequest( const HttpRequest& req)
{
    HttpResponse    res;
    RouteCfg*       route = find_route(req, _cfg.routes);
    std::string     path;

    // Check if a valid route has been found
    if (!route) return response_error( req, res, _cfg, route, 404);

    if ( route->is_redirect ) {
        res.set_status( 301 );
        res.set_header("Location" , route->redirect_to );
        return res;
    }

    path = get_path(req, route);

    // Check if file exists
    if (!is_directory(path) && ::access(path.c_str(), F_OK) < 0)
        return response_error(req, res, _cfg, route, 404);

    // Check if we have access to file
    else if (::access(path.c_str(), O_RDONLY) < 0)
        return response_error(req, res, _cfg, route, 403);

    // Check if requested method is available
    else if (!route->accepted_methods.empty() && !is_accepted_method(route, req.method()))
        return response_error(req, res, _cfg, route, 405);

    // Handle CGI
    else if (ServerConfig::isCgiScript(path))
        return response_cgi(req, path, res, _cfg, route);

    // Handle GET method
    else if (req.method() == "GET")
        return response_get(req, path, res, _cfg, route);

    // Handle HEAD method
    else if (req.method() == "HEAD")
        return response_head(req, path, res, _cfg, route);

    // Handle DELETE method
    else if (req.method() == "DELETE")
        return response_delete(req, path, res, _cfg, route);

    else
    {
        std::cout << "Returning response error" << std::endl;
        return response_error(req, res, _cfg, route, 501);
    }
}

ServerCfg&  Server::cfg()
{
    return _cfg;
}
// END: Class Functions


// BEGIN: Helper Functions
RouteCfg*   find_route(const HttpRequest& req, std::vector<RouteCfg>& routes)
{
    std::pair<std::size_t, RouteCfg*>   route_match(0, NULL);

    for(std::vector<RouteCfg>::iterator i = routes.begin(); i != routes.end(); ++i) {
	    std::string	available_route = i->route_path;
	    if ( available_route[available_route.size() - 1] == '/' ) available_route.substr( 0, available_route.size() - 1 );
	    
	    std::string::size_type  p = req.target().find( available_route );
	    std::string::size_type  q = req.target().find( available_route  + "/" );

	    if (p == 0) {
	    	if ( req.target() != available_route && q != 0 && available_route != "/")
			available_route = "";
		if ( available_route.length() > route_match.first )
            		route_match = std::pair<int, RouteCfg*>(available_route.length(), &(*i));
	    }
    }

    return route_match.second;
}

HttpResponse    list_dir_res(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route)
{
    res.body().clear();
    
    std::ifstream   file(DIRLISTING);
    std::string     line_buff;

    if (!file.is_open())
        return response_error(req, res, _cfg, route, 500);
    
    std::string                 items;
    std::vector<std::string>    dir_listing = list_dir(path);

    for (size_t i = 0; i < dir_listing.size(); i++)
        items.append("<li><a href=\"" + removeSlashDups(req.target() + "/") + dir_listing[i] + "\">" + dir_listing[i] + "</a></li>");

    // Replace all occurs of [DIR] & [ITEMS] with dir name and dir listing
    while(std::getline(file, line_buff))
    {
        replace_occurrence(line_buff, "[DIR]", req.target());
        replace_occurrence(line_buff, "[ITEMS]", items);
        res.body().append(line_buff);
    }

    res.set_status(200);
    res.set_header("Content-Type", "text/html");
    file.close();

    return res;
}

void    set_res_cgi_headers(HttpResponse& res, std::string response)
{
    std::string             line;
    int                     i;
    std::string::size_type  startPos = 0;
    std::string::size_type  endPos;

    while ((endPos = response.find('\n', startPos)) != std::string::npos)
    {
        line = trimSpace(response.substr(startPos, endPos - startPos));

        if (line.empty())
        {
            startPos = response.find('\n', endPos + 1);
            res.body().append(response.substr(startPos));
            break;

        } else if (startsWith(line, "HTTP/"))
        {
            int         statusCode = 0;
            std::string statusMessage = "";

            // Get the 'i' variable to the index of the first char of the status code (3 chars)
            i = line.find_first_of(' ');
            i = line.find_first_not_of(' ', i);

            if (line.length() >= i + 3)
            {
                statusCode = ParserUtils::atoi(line.substr(i, 3));

                i += 3;
                while (line[i] == ' ')
                    i++;

                statusMessage = line.substr(i);
            }

            res.set_status(statusCode, statusMessage);
        }

        i = line.find(':');
        if ((i != 0) && (i != line.length() - 1) && (i != std::string::npos))
            res.set_header(line.substr(0, i), trimSpace(line.substr(i + 1)));

        startPos = endPos + 1;
    }
}

std::string executeScript(std::string executablePath, std::string path, std::string body, char **envp)
{
    int     pipe_in[2];
    int     pipe_out[2];
    pid_t   _cgi_pid;
    char    **argv;

    argv = new char *[3];
    argv[0] = new char[executablePath.length() + 1];
    argv[1] = new char[path.length() + 1];

    strcpy(argv[0], executablePath.c_str());
    strcpy(argv[1], path.c_str());

    argv[2] = NULL;

    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0)
    {
        std::cout << "ERROR: Failed to create pipes" << std::endl;
        return "";
    }

    _cgi_pid = fork();

    // Child process
    if (_cgi_pid == 0)
    {
        close(pipe_in[1]);
        close(pipe_out[0]);
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);

        // Execute the CGI script
        execve(argv[0], argv, envp);

        std::cout << "ERROR: Failed to execute CGI script : " << std::endl;
        exit(1);
    }
    // Parent process
    else if (_cgi_pid > 0)
    {
        close(pipe_in[0]);
        close(pipe_out[1]);

        // Write the request body to the child process
        write(pipe_in[1], body.c_str(), body.length());
        close(pipe_in[1]);

        // Wait for the child process to complete
        int status;
        waitpid(_cgi_pid, &status, 0);

        if (WIFEXITED(status))
        {
            // Child process exited normally
            WEXITSTATUS(status);

            // Read the response from the child process
            char				buffer[1024];
            int					bytes_read;
            std::stringstream	ss;
            while ((bytes_read = read(pipe_out[0], buffer, sizeof(buffer))) > 0)
                ss << buffer;

            close(pipe_out[0]);

            return ss.str();
        }
        else
            std::cout << "ERROR: Child process did not exit normally" << std::endl;
    }
    else
    {
        std::cout << "ERROR: Failed to fork" << std::endl;
        return "";
    }

    return "";
}

char            **get_cgi_headers(const HttpRequest& req, std::string path)
{
    std::map<std::string, std::string>  headers;
    int i_pos;

    headers["AUTH_TYPE"] = "Basic";
    headers["GATEWAY_INTERFACE"] = "CGI/1.1";
    headers["HTTP_COOKIE"] = req.headers("Cookie");
    headers["REDIRECT_STATUS"] = "200";
    headers["REMOTE_ADDR"] = req.headers("Host");
    headers["REQUEST_METHOD"] = req.method();

    i_pos = find_delimiter(path, "/cgi-bin/");
    headers["PATH_INFO"] = "";
    headers["PATH_TRANSLATED"] = path;
    headers["REQUEST_URI"] = (i_pos < 0 ? "" : path.substr(i_pos, path.size()));
    headers["SCRIPT_NAME"] = (i_pos < 0 ? "" : path.substr(i_pos + 1, path.size()));
    headers["SCRIPT_FILENAME"] = (i_pos < 0 ? "" : path.substr(i_pos + 9, path.size()));

    i_pos = find_delimiter(req.headers("Host"), ":");
    headers["SERVER_NAME"] = (i_pos > 0 ? req.headers("Host").substr(0, i_pos) : "");
    headers["SERVER_PORT"] = (i_pos > 0 ? req.headers("Host").substr(i_pos + 1, req.headers("Host").size()) : "");
    headers["SERVER_PROTOCOL"] = "HTTP/1.1";
    headers["SERVER_SOFTWARE"] = "AMANIX";

    if (req.method() == "POST")
    {
        headers["CONTENT_LENGTH"] = req.headers("Content-Length");
        headers["CONTENT_TYPE"] = req.headers("Content-Type");
    }

    char    **envp = new char *[headers.size() + 1];
    i_pos = 0;
    std::map<std::string, std::string>::iterator    it;
    for (it = headers.begin(); it != headers.end(); ++it, ++i_pos)
    {
        std::string value = it->first + "=" + it->second;
        envp[i_pos] = new char[value.length() + 1];
        strcpy(envp[i_pos], value.c_str());
    }

    envp[i_pos] = NULL;

    return envp;
}

void    delete_cgi_headers(char **envp)
{
    for (int i = 0; envp[i] != NULL; i++)
        delete[] envp[i];

    delete[] envp;
}

HttpResponse    response_cgi(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route)
{
    char    **envp = get_cgi_headers(req, path);

    std::string executablePath = ServerConfig::getExecutablePath(path);
    std::string result = executeScript(executablePath, path, req.body(), envp);

    delete_cgi_headers(envp);

    set_res_cgi_headers(res, result);

    return res;
}

HttpResponse    response_get(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route) {
    switch (index_path(req, route, path)) {
        case AUTOINDEX:
            return list_dir_res(req, path, res, _cfg, route);

        case INVALIDPATH:
            return response_error(req, res, _cfg, route, 404);
    }

    std::ifstream   file(path.c_str());
    char buff [BUFFER_SIZE];

    if (!file.is_open())
        return response_error(req, res, _cfg, route, 500);

    std::ostringstream ss;

    ss << file.rdbuf();
    file.close();

    res.body() = ss.str();

    res.set_status(200);
    res.set_header("Content-Type", ServerConfig::getMimeType(path));

    return res;
}

HttpResponse    response_head(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route) {
    switch (index_path(req, route, path)) {
        case VALIDPATH:
            res.set_status(200);
            res.set_header("Content-type", ServerConfig::getMimeType(path));
            return res;

        case AUTOINDEX:
            res.set_status(200);
            res.set_header("Content-type", ServerConfig::getMimeType(DIRLISTING));
            return res;

        case INVALIDPATH:
            return response_error(req, res, _cfg, route, 404);
    }

    return res;
}

HttpResponse    response_delete(const HttpRequest& req, std::string path, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route) {
    std::string executablePath = ServerConfig::getExecutablePath("/cgi-bin/form-delete.rb");
    std::string deletePath = get_path("/cgi-bin/form-delete.rb", route);
    std::string body = "filename=";
    body.append(path);

    std::string result = executeScript(executablePath, deletePath, body, NULL);

    set_res_cgi_headers(res, result);

    return res;
}

HttpResponse    response_error(const HttpRequest& req, HttpResponse& res, ServerCfg& _cfg, RouteCfg* route, const int statusCode) {
    std::map<short, std::string>::const_iterator    it = _cfg.error_pages.find(statusCode);
    
    if (it != _cfg.error_pages.end()) {
    	std::string     path = get_path(it->second, _cfg);
    	std::ifstream   file(path.c_str());
    	std::string     buff(BUFFER_SIZE, '\0');

		if (is_file(path) && file.is_open()) {
			while(file.read(&buff[0], BUFFER_SIZE).gcount() > 0)
				res.body().append(buff, 0, file.gcount());
			
			std::ostringstream  ss;
            		ss << res.body().length();

            		res.set_status(200);
            		res.set_header("Content-Type", ServerConfig::getMimeType(path));
        	} else if (statusCode == 500)
			res.set_status(500);
		else
			return response_error(req, res, _cfg, route, 500);
	}
    else {
		res.body().append( "\
				<html><head><title>Error</title></head>\
				<body style=\"\
				display: flex;\
				justify-content: center;\
				align-items: center;\
				height: 100vh; margin: 0;\"\
				><h1>Error</h1></body></html>\
				" );
        res.set_status( statusCode );
        res.set_header("Content-Type", "text/html");
		return res;
	}
	return res;
}
// END: Helper Functions
