NAME=nim

nim: main.c
	$(CC) main.c -Wall -Wextra -pedantic -std=c11 -o $(NAME)

clean:
	rm $(NAME)
