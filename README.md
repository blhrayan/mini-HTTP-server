The mini HTTP server is built and works. I compiled it with -Wall -Wextra and got no warnings, then tested it with curl. Normal pages, 404s, blocked ../ attempts, POST requests and 20 parallel requests all behaved correctly, and no zombie processes were left behind.

The project is server.c plus a small www/ folder with index.html, about.html and style.css. Keep the www/ folder next to server.c.

Run it

bash
gcc -o server server.c
./server                # port 8080, serves ./www
./server 3000 ./www     # custom port and folder
