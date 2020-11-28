#include <stdio.h>

int foo1(int, int);
int foo3(int, int);
typedef struct {
   float r;
   float g;
   float b;
   int cluster;
   float distance;
} RgbPixel;




int main() {
	#pragma taffo a main "scalar(disabled range(-3000, 3000))"
	int a;
	int b, c1, c2;
	#pragma taffo pixel main "scalar(disabled range(-4000, 4000))"
	RgbPixel pixel;

	#pragma taffo pixel main // must report error
	#pragma taffo pixel // must report error
	#pragma taffo //must report error
	#pragma taffo pixel "scalar example" //must report error
	#pragma taffo pixel main "scalar example" additional //must report error and generate warning
	#pragma taffo main pixel "scalar example" //no error is reported
	a = 7;
	b = 5;
	c1 = foo1(a, b);
	c2 = foo3(a, b);
	printf("C1 is: %d\n", c1);
	printf("C2 is: %d\n", c2);
	return 0;
}


int foo1(int c, int b){
	int a = 1;
	int d = a;
	return c +b + a;
}

int foo3(int a, int b){
	return a-b;
}
