The server-side uptime logging code requires two external
php packages: jpgraph and twilio-php

Go find those on google and add them to the include path.

It also requires php-sqlite3, so make sure that is available.

It also requires a directory structure to be set up based on
the virtual host names.

This directory needs to be writeable by the www process.
data/<host-name>/

The sqlite3 database contains a list of watchdogs that need
to be pet or SMS messages will get sent. The php script will
create the database (if it has write access to the path).

I have two entries: one to notify me if the pilot light monitor
fails to check in, the other if another computer on my network
fails to check in. This lets me know if it is a home-network
problem or a pilot-light-monitor problem.
