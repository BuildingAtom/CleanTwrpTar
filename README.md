# CleanTwrpTar
This is a single file C program primarily made to remove leaked strings from TWRP backup tars, but also intended to be somewhat capable of cleaning other tars that have strings leaked between data blocks. This program was made in response to https://github.com/TeamWin/Team-Win-Recovery-Project/issues/964. 

## Background
Creating a program like this was also used as a challenge to my friend who I helped get into programming a couple of years ago. You can check out his solution at https://github.com/venerjoel99/TarProject. We both approached this problem from similar, yet different angles. I would consider trying his program before mine since I consider my algorithm a bit more experimental. Also, my code stil has a ways to go before I can consider it fully functional. There was also a basic scoring to this challenge:
* 10 points for every day it's finished before the other person.
* 10 points for every minute faster the program runs
* 50 points for the program successfully cleaning the file
* 20 points for whichever program is more flexible/extensible

As of right now, the programs have not been compared, so he is at a 50 point lead for finishing 5 days ahead of me (declared finished at commit [e1b2a45](https://github.com/venerjoel99/TarProject/tree/e1b2a454f936778193803d35e5b190036bf11f2a)).

## Compiler information
Just make sure you compile with the C11 standard. I did my best to keep everything within the standard.

## Usage information
```CleanTwrpTar [ARGUMENTS] [input-file] [output-file]```

For now, `CleanTwrpTar -v input-file.tar output-file.tar` will work just fine. The maximum filesize this program can currently handle is 2GB.
