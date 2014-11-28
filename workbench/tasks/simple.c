int atoi(const char * s){
	int i,res = 0;
	for(i = 0; s[i]; i++)
		res = 10*res + s[i] - '0';
	return res;
}
int main(int argc, const char * argv[]){
	int i,sum = 0;
	for(i = 1; i < argc; i++)
		sum += atoi(argv[i]);
	return 0;
}
