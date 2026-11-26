/* A code resource for tests/run.sh: codelink.py --code-resource with
   start-coderes.S. The caller would call main with its own arguments. */
static const char kName[] = "code resource";
static const char *gName = kName; /* an absolute pointer to relocate */

/* Gives the length of the name, through the relocated pointer. */
int main(void) {
  int n = 0;
  while (gName[n]) n++;
  return n;
}
