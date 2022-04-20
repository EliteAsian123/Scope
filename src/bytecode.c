#include "bytecode.h"

// TODO: Arrays shouldn't use ValueHolders
// TODO: Arrays don't properly dispose their elements

// TODO: Proper accessors `.`
// TODO: Redo `repeat`
// TODO: Utilities
// TODO: Types
// TODO: size_t for goto and stuff
// TODO: static checking

#define basicOperation(op)                                                                 \
	b = pop();                                                                             \
	a = pop();                                                                             \
	if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {                                  \
		push((Object){.type = type(TYPE_INT), .v.v_int = a.v.v_int op b.v.v_int});         \
	} else if (a.type.id == TYPE_FLOAT && b.type.id == TYPE_FLOAT) {                       \
		push((Object){.type = type(TYPE_FLOAT), .v.v_float = a.v.v_float op b.v.v_float}); \
	} else if (a.type.id == TYPE_LONG && b.type.id == TYPE_LONG) {                         \
		push((Object){.type = type(TYPE_LONG), .v.v_long = a.v.v_long op b.v.v_long});     \
	} else {                                                                               \
		ierr("Invalid types for `" #op "`.");                                              \
	}

#define boolOperation(op)                                                               \
	b = pop();                                                                          \
	a = pop();                                                                          \
	if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {                               \
		push((Object){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int op b.v.v_int});     \
	} else if (a.type.id == TYPE_FLOAT && b.type.id == TYPE_FLOAT) {                    \
		push((Object){.type = type(TYPE_BOOL), .v.v_int = a.v.v_float op b.v.v_float}); \
	} else if (a.type.id == TYPE_LONG && b.type.id == TYPE_LONG) {                      \
		push((Object){.type = type(TYPE_BOOL), .v.v_int = a.v.v_long op b.v.v_long});   \
	} else {                                                                            \
		ierr("Invalid types for `" #op "`.");                                           \
	}

#define toStr(x, s)                                                                                      \
	int length = snprintf(NULL, 0, s, x) + 1;                                                            \
	char* str = malloc(length);                                                                          \
	snprintf(str, length, s, x);                                                                         \
	push((Object){.type = type(TYPE_STR), .v.v_string = cstrToStr(str), .referenceId = basicReference}); \
	free(str);

#define ierr(...)                                          \
	fprintf(stderr, "Interpret Error: " __VA_ARGS__ "\n"); \
	exit(-1);

#define iwarn(...) fprintf(stdout, "Interpret Warning: " __VA_ARGS__ "\n");

#define jump(loc) i = loc - 1;

#define curInstBuf instbuffer[instbufferCount - 1]

typedef struct {
	NamedObject* vars;
	size_t varsCount;
} ObjectList;

typedef struct {
	ObjectList* o;
} CallFrame;

typedef struct {
	Inst* insts;
	size_t instsCount;
} InstBuffer;

// Flags
static bool showCount = false;
static bool showDisposeInfo = false;

// Interpret stage
static CallFrame frames[STACK_SIZE];
static size_t framesCount;

// Interpret & Parse stage
static Object stack[STACK_SIZE];
static size_t stackCount;

// Parse stage
static int locstack[STACK_SIZE];
static size_t locstackCount;

// Parse stage
static int loopstack[STACK_SIZE];
static size_t loopstackCount;

// Interpret stage
static char* argstack[STACK_SIZE];
static size_t argstackCount;

// Interpret stage
static FuncPointer* funcs;
static size_t funcsCount;

// Parse stage
static InstBuffer instbuffer[STACK_SIZE];
static size_t instbufferCount;
static bool isInBuffer;

// Interpret stage
static ReferenceInfo* refs;
static size_t refsCount;

void pushFrame(CallFrame frame) {
	frames[framesCount++] = frame;

	if (framesCount >= STACK_SIZE) {
		ierr("Call frame stack overflowed.");
	}
}

CallFrame popFrame() {
	return frames[--framesCount];
}

void push(Object obj) {
	stack[stackCount++] = obj;

	if (stackCount >= STACK_SIZE) {
		ierr("Internal stack size exceeded.");
	}
}

Object pop() {
	return stack[--stackCount];
}

Object stackRead() {
	return stack[stackCount - 1];
}

void pushLoc() {
	locstack[locstackCount++] = instsCount;

	if (locstackCount >= STACK_SIZE) {
		ierr("Location stack size exceeded.");
	}
}

void pushSLoc(int loc) {
	locstack[locstackCount++] = loc;
}

int popLoc() {
	return locstack[--locstackCount];
}

int readLoc() {
	return locstack[locstackCount - 1];
}

void pushLoop() {
	loopstack[loopstackCount++] = instsCount;

	if (loopstackCount >= STACK_SIZE) {
		ierr("Loop stack size exceeded.");
	}
}

int popLoop() {
	return loopstack[--loopstackCount];
}

int readLoop() {
	return loopstack[loopstackCount - 1];
}

void pushArg(char* arg) {
	argstack[argstackCount++] = arg;

	if (argstackCount >= STACK_SIZE) {
		ierr("Location stack size exceeded.");
	}
}

char* popArg() {
	return argstack[--argstackCount];
}

static int pushFunc(int loc, TypeInfo type) {
	FuncPointer f;
	f.location = loc;
	f.type = type;
	f.argsLen = type.argsLen - 1;
	f.args = malloc(sizeof(char*) * f.argsLen);
	for (int i = f.argsLen - 1; i >= 0; i--) {
		f.args[i] = popArg();
	}

	funcsCount++;
	funcs = realloc(funcs, sizeof(FuncPointer) * funcsCount);
	funcs[funcsCount - 1] = f;

	return funcsCount - 1;
}

static void delVarAtIndex(ObjectList* o, size_t i) {
	NamedObject var = o->vars[i];

	// Change ref counter
	if (isDisposable(var.o.type.id)) {
		refs[var.o.referenceId].counter--;

		if (showDisposeInfo) {
			printf("(-) %s: %d\n", var.name, refs[var.o.referenceId].counter);
		}
	}

	// Free
	free(var.name);
	freeTypeInfo(var.o.type);

	// Ripple down
	for (int j = i; j < o->varsCount - 1; j++) {
		o->vars[j] = o->vars[j + 1];
	}

	// Resize
	o->varsCount--;
	o->vars = (NamedObject*) realloc(o->vars, sizeof(NamedObject) * o->varsCount);
}

static void delVar(ObjectList* o, const char* n) {
	for (size_t i = 0; i < o->varsCount; i++) {
		if (strcmp(o->vars[i].name, n) == 0) {
			delVarAtIndex(o, i);
		}
	}
}

static void setVar(ObjectList* o, NamedObject obj) {
	delVar(o, obj.name);

	if (isDisposable(obj.o.type.id)) {
		refs[obj.o.referenceId].counter++;

		if (showDisposeInfo) {
			printf("(+) %s: %d\n", obj.name, refs[obj.o.referenceId].counter);
		}
	}

	o->varsCount++;
	o->vars = realloc(o->vars, sizeof(NamedObject) * o->varsCount);
	o->vars[o->varsCount - 1] = obj;
}

static void dupVar(ObjectList* o, NamedObject obj) {
	NamedObject d = obj;
	d.name = strdup(obj.name);
	d.o.type = dupTypeInfo(obj.o.type);

	setVar(o, d);
}

static NamedObject getVar(ObjectList* o, const char* n) {
	for (size_t i = 0; i < o->varsCount; i++) {
		if (strcmp(o->vars[i].name, n) == 0) {
			return o->vars[i];
		}
	}

	printf("Unknown variable `%s`.\n", n);
	ierr("Use of undeclared variable.");
}

static bool isVar(ObjectList* o, const char* n) {
	for (size_t i = 0; i < o->varsCount; i++) {
		if (strcmp(o->vars[i].name, n) == 0) {
			return true;
		}
	}

	return false;
}

static void disposeObjectList(ObjectList* o) {
	for (size_t i = 0; i < o->varsCount; i++) {
		free(o->vars[i].name);
		freeTypeInfo(o->vars[i].o.type);
	}
	free(o->vars);
}

void pushInst(Inst i, int scope) {
	i.scope = scope;
	i.location = instsCount;

	if (!isInBuffer) {
		instsCount++;
		insts = realloc(insts, sizeof(Inst) * instsCount);
		insts[instsCount - 1] = i;
	} else {
		curInstBuf.instsCount++;
		curInstBuf.insts = realloc(curInstBuf.insts, sizeof(Inst) * curInstBuf.instsCount);
		curInstBuf.insts[curInstBuf.instsCount - 1] = i;
	}
}

void setInst(Inst i, int loc, int scope) {
	if (isInBuffer) {
		fprintf(stderr, "Parse Error: `setInst` cannot be used in a move buffer.\n");
		exit(-1);
	}

	i.scope = scope;
	i.location = loc;

	insts[loc] = i;
}

void pushInstAt(Inst i, int loc, int scope) {
	if (isInBuffer) {
		fprintf(stderr, "Parse Error: `pushInstAt` cannot be used in a move buffer.\n");
		exit(-1);
	}

	i.scope = scope;
	i.location = loc;

	instsCount++;
	insts = realloc(insts, sizeof(Inst) * instsCount);

	for (size_t i = instsCount - 1; i > loc; i--) {
		insts[i] = insts[i - 1];
	}
	insts[loc] = i;
}

void startMoveBuffer() {
	if (isInBuffer) {
		fprintf(stderr, "Parse Error: We are already in a move buffer.\n");
		exit(-1);
	}

	instbufferCount++;
	if (instbufferCount >= STACK_SIZE) {
		fprintf(stderr, "Parse Error: Move buffer stack size exceeded.\n");
	}

	curInstBuf.insts = malloc(0);
	curInstBuf.instsCount = 0;
	isInBuffer = true;
}

void endMoveBuffer() {
	if (!isInBuffer) {
		fprintf(stderr, "Parse Error: We are not in a move buffer.\n");
		exit(-1);
	}

	isInBuffer = false;
}

void putMoveBuffer(int scope) {
	if (isInBuffer) {
		fprintf(stderr, "Parse Error: Cannot `putMoveBuffer` when we are in one!\n");
		exit(-1);
	}

	if (instbufferCount <= 0) {
		fprintf(stderr, "Parse Error: Move buffer stack is empty!\n");
		exit(-1);
	}

	for (size_t i = 0; i < curInstBuf.instsCount; i++) {
		pushInst(curInstBuf.insts[i], scope);
	}

	free(curInstBuf.insts);
	instbufferCount--;
}

size_t createReference(ReferenceInfo info) {
	refsCount++;
	refs = realloc(refs, sizeof(ReferenceInfo) * refsCount);
	refs[refsCount - 1] = info;
	return refsCount - 1;
}

void removeReference(size_t id) {
	// TODO
}

static bool stringEqual(String a, String b) {
	if (a.len != b.len) {
		return false;
	}

	for (size_t i = 0; i < a.len; i++) {
		if (a.chars[i] != b.chars[i]) {
			return false;
		}
	}

	return true;
}

void bc_init() {
	insts = malloc(0);
	instsCount = 0;

	stackCount = 0;

	funcs = malloc(0);
	funcsCount = 0;
}

static void instDump(size_t i) {
	const char* instName;
	bool isStringArg = false;

	// clang-format off
	static_assert(_INSTS_ENUM_LEN == 37, "Update bytecode strings.");
	switch (insts[i].inst) {
		case LOAD:
			instName = "load";
			isStringArg = insts[i].type.id == TYPE_STR;
			break;
		case LOADT: 	instName = "loadt"; 						break;
		case LOADV: 	instName = "loadv"; 	isStringArg = true; break;
		case LOADA: 	instName = "loada"; 	isStringArg = true; break;
		case SAVEV: 	instName = "savev"; 	isStringArg = true; break;
		case RESAVEV: 	instName = "resavev"; 	isStringArg = true; break;
		case SAVEF: 	instName = "savef"; 						break;
		case CALLF: 	instName = "callf"; 	isStringArg = true;	break;
		case ENDF: 		instName = "endf"; 							break;
		case EXTERN: 	instName = "extern"; 						break;
		case APPENDT: 	instName = "appendt"; 						break;
		case ARRAYI: 	instName = "arrayi"; 						break;
		case ARRAYIL: 	instName = "arrayil"; 						break;
		case ARRAYS: 	instName = "arrays"; 	isStringArg = true; break;
		case ARRAYG: 	instName = "arrayg"; 	isStringArg = true; break;
		case ARRAYL: 	instName = "arrayl"; 						break;
		case SWAP: 		instName = "swap"; 							break;
		case NOT: 		instName = "not"; 							break;
		case AND: 		instName = "and"; 							break;
		case OR: 		instName = "or"; 							break;
		case ADD: 		instName = "add"; 							break;
		case SUB: 		instName = "sub"; 							break;
		case MUL: 		instName = "mul"; 							break;
		case DIV: 		instName = "div"; 							break;
		case MOD: 		instName = "mod"; 							break;
		case POW: 		instName = "pow"; 							break;
		case NEG: 		instName = "neg"; 							break;
		case EQ: 		instName = "eq"; 							break;
		case GT: 		instName = "gt"; 							break;
		case LT: 		instName = "lt"; 							break;
		case GTE: 		instName = "gte"; 							break;
		case LTE: 		instName = "lte"; 							break;
		case CAST: 		instName = "cast"; 							break;
		case GOTO: 		instName = "goto"; 							break;
		case IFN:	 	instName = "ifn"; 							break;
		case THROW:	 	instName = "throw";		isStringArg = true;	break;
		default: 		instName = "?"; 							break;
	}
	// clang-format on

	if (isStringArg) {
		printf("[%ld, %d] %s: \"%s\", %s\n", i, insts[i].scope, instName,
			   (char*) insts[i].a.v_ptr, typestr(insts[i].type.id));
	} else {
		printf("[%ld, %d] %s: %d, %s\n", i, insts[i].scope, instName,
			   insts[i].a.v_int, typestr(insts[i].type.id));
	}
}

static void bc_dump() {
	for (size_t i = 0; i < instsCount; i++) {
		instDump(i);
	}
}

static void readByteCode(size_t frameIndex, size_t start) {
	int lastKnownScope = 0;
	CallFrame frame = frames[frameIndex];

	for (size_t i = start; i < instsCount; i++) {
		if (showCount) {
			instDump(i);
		}

		// Adjust scope

		int curScope = insts[i].scope;

		if (curScope < lastKnownScope) {
			for (size_t v = 0; v < frame.o->varsCount; v++) {
				NamedObject obj = frame.o->vars[v];

				if (obj.scope <= curScope) {
					continue;
				}

				if (showDisposeInfo) {
					printf("Deleting : (%d > %d) `%s`\n", obj.scope, curScope, obj.name);
				}

				delVarAtIndex(frame.o, v);

				if (isDisposable(obj.o.type.id)) {
					if (refs[obj.o.referenceId].counter <= 0) {
						if (showDisposeInfo) {
							printf("Disposing: `%ld`\n", obj.o.referenceId);
						}

						dispose(obj.o.type, obj.o.v);
					}
				}

				v--;
			}
		}
		lastKnownScope = curScope;

		NamedObject obj;
		Object sobj;
		Array arr;
		int s;

		Object a, b, c;
		a.type = type(TYPE_VOID);
		b.type = type(TYPE_VOID);
		c.type = type(TYPE_VOID);

		static_assert(_INSTS_ENUM_LEN == 37, "Update bytecode interpreting.");
		switch (insts[i].inst) {
			case LOAD:
				sobj = (Object){
					.type = dupTypeInfo(insts[i].type),
				};

				if (insts[i].type.id == TYPE_STR) {
					// We must convert string literals because they are c-strings
					sobj.v.v_string = cstrToStr(insts[i].a.v_ptr);
				} else {
					sobj.v = insts[i].a;
				}

				if (isDisposable(insts[i].type.id)) {
					sobj.referenceId = basicReference;
				}

				push(sobj);

				break;
			case LOADT:
				push((Object){.type = dupTypeInfo(insts[i].type)});
				break;
			case LOADV:
				obj = getVar(frame.o, insts[i].a.v_ptr);

				if (obj.o.fromArgs && obj.o.type.id == TYPE_FUNC) {
					ierr("Functions from arguments can only be called.");
				}

				obj.o.type = dupTypeInfo(obj.o.type);
				push(obj.o);
				break;
			case LOADA:
				pushArg(strdup(insts[i].a.v_ptr));
				break;
			case SAVEV:
				b = pop();
				a = pop();

				if (isVar(frame.o, insts[i].a.v_ptr)) {
					if (getVar(frame.o, insts[i].a.v_ptr).scope == curScope) {
						ierr("Redefinition of existing variable in the same scope.");
					}
				}

				if (a.type.id != TYPE_UNKNOWN) {
					if (!typeInfoEqual(b.type, a.type)) {
						ierr("Declaration type doesn't match expression.");
					}
				}

				obj = (NamedObject){
					.name = strdup(insts[i].a.v_ptr),
					.scope = curScope,
					.o = objdup(b),
				};

				setVar(frame.o, obj);
				break;
			case RESAVEV:
				a = pop();

				if (a.type.id == TYPE_FUNC) {
					ierr("Functions can't be reassigned at the moment.");
				}

				obj = getVar(frame.o, insts[i].a.v_ptr);
				if (obj.o.fromArgs) {
					ierr("Cannot assign to an argument.");
				}

				if (!typeInfoEqual(a.type, obj.o.type)) {
					ierr("Assignment type doesn't match expression.");
				}

				obj = (NamedObject){
					.name = strdup(insts[i].a.v_ptr),
					.scope = obj.scope,
					.o = objdup(a),
				};
				setVar(frame.o, obj);
				break;
			case SAVEF:
				a = pop();

				push((Object){.type = dupTypeInfo(a.type), .v.v_int = pushFunc(i + 1, a.type)});
				jump(insts[i].a.v_int);

				break;
			case CALLF:
				obj = getVar(frame.o, (char*) insts[i].a.v_ptr);

				bool dropOutput = false;
				if (insts[i].type.id == TYPE_UNKNOWN &&
					obj.o.type.args[0].id == TYPE_VOID) {
					ierr("Invoke expression cannot be referencing a void function.");
				} else if (insts[i].type.id == TYPE_VOID &&
						   obj.o.type.args[0].id != TYPE_VOID) {
					dropOutput = true;
				}

				FuncPointer f = funcs[obj.o.v.v_int];
				s = insts[f.location].scope;

				ObjectList fo;
				fo.vars = malloc(0);
				fo.varsCount = 0;

				if (obj.o.fromArgs) {
					popFrame();
				}
				CallFrame funcFrame = frames[framesCount - 1];

				// Duping may not even be required?
				// TODO: Optimize
				for (size_t v = 0; v < funcFrame.o->varsCount; v++) {
					NamedObject vobj = funcFrame.o->vars[v];
					if (vobj.scope < s) {
						dupVar(&fo, vobj);
					}
				}

				for (int v = f.argsLen - 1; v >= 0; v--) {
					Object e = pop();

					if (!typeInfoEqual(f.type.args[v + 1], e.type)) {
						printf("`%d` != `%d` in `%s`\n", e.type.id, f.type.args[v + 1].id, (char*) insts[i].a.v_ptr);
						ierr("Type mismatch in function arguments.");
					}

					NamedObject vo = (NamedObject){
						.name = strdup(f.args[v]),
						.scope = s,
						.o = objdup(e),
					};
					setVar(&fo, vo);
				}

				pushFrame((CallFrame){.o = &fo});
				readByteCode(framesCount - 1, f.location);
				popFrame();

#define skipdelete         \
	delVarAtIndex(&fo, v); \
	v--;                   \
	continue

				// TODO: Optimize
				for (size_t v = 0; v < fo.varsCount; v++) {
					NamedObject vobj = fo.vars[v];

					// Temporary for now
					if (vobj.o.type.id == TYPE_FUNC) {
						skipdelete;
					}

					// Skip if it is a local variable
					if (vobj.scope >= s) {
						skipdelete;
					}

					// Skip if the current frame doesn't contain the variable
					if (!isVar(funcFrame.o, vobj.name)) {
						skipdelete;
					}

					// Skip and delete if the variable is an argument
					if (obj.o.fromArgs) {
						skipdelete;
					}

					// Get the var in the frame
					NamedObject oobj = getVar(funcFrame.o, vobj.name);

					// Skip if the types are not equal
					if (!typeInfoEqual(vobj.o.type, oobj.o.type)) {
						skipdelete;
					}

					// Skip if the scopes don't match
					if (vobj.scope != oobj.scope) {
						skipdelete;
					}

					// Update the variable outside of the frame
					dupVar(funcFrame.o, vobj);
				}

#undef skipdelete

				disposeObjectList(&fo);

				if (obj.o.fromArgs) {
					pushFrame(frame);
				}

				if (dropOutput) {
					pop();
				}

				break;
			case ENDF:
				// RETURN not break
				return;
			case EXTERN:
				a = pop();
				externs[a.v.v_int]();

				break;
			case APPENDT:
				b = pop();
				a = pop();

				a.type.argsLen++;
				if (a.type.args == NULL) {
					a.type.args = malloc(sizeof(TypeInfo));
					a.type.args[0] = dupTypeInfo(b.type);
				} else {
					a.type.args = realloc(a.type.args, sizeof(TypeInfo) * a.type.argsLen);
					a.type.args[a.type.argsLen - 1] = dupTypeInfo(b.type);
				}

				push(a);

				// We don't want this to get freed
				a.type = type(TYPE_VOID);

				break;
			case ARRAYI:  // `new a[b]`
				b = pop();
				a = pop();

				if (b.type.id != TYPE_INT || b.v.v_int < 0) {
					ierr("Expected positive int in array initilization.");
				}

				// Initilize the new stack element
				sobj = (Object){
					.type = type(TYPE_ARRAY),
					.referenceId = basicReference,
				};

				// Convert type into array type
				sobj.type.argsLen = 1;
				sobj.type.args = malloc(sizeof(TypeInfo));
				sobj.type.args[0] = dupTypeInfo(a.type);

				// Initilize the array
				sobj.v.v_array.arr = malloc(sizeof(ValueHolder) * b.v.v_int);
				sobj.v.v_array.len = b.v.v_int;

				// Populate array
				for (int i = 0; i < b.v.v_int; i++) {
					sobj.v.v_array.arr[i] = createDefaultType(a.type);
				}

				// Push
				push(sobj);

				break;
			case ARRAYIL:;	// `new a[] { n0, n1, n2, ... }`
				Object* arrayList = malloc(sizeof(Object) * insts[i].a.v_int);
				for (int j = 0; j < insts[i].a.v_int; j++) {
					arrayList[j] = pop();
				}

				a = pop();

				// Check types
				for (int j = 0; j < insts[i].a.v_int; j++) {
					if (!typeInfoEqual(arrayList[j].type, a.type)) {
						ierr("An element in the array initialization does not match the array type.");
					}
				}

				// Initilize the new stack element
				sobj = (Object){
					.type = type(TYPE_ARRAY),
					.referenceId = basicReference,
				};

				// Convert type into array type
				sobj.type.argsLen = 1;
				sobj.type.args = malloc(sizeof(TypeInfo));
				sobj.type.args[0] = dupTypeInfo(a.type);

				// Convert the object list to a ValueHolder list
				sobj.v.v_array.arr = malloc(sizeof(ValueHolder) * insts[i].a.v_int);
				sobj.v.v_array.len = insts[i].a.v_int;

				// Populate
				for (int j = 0; j < insts[i].a.v_int; j++) {
					sobj.v.v_array.arr[j] = arrayList[insts[i].a.v_int - j - 1].v;
				}

				// Push
				push(sobj);

				// Free
				free(arrayList);

				break;
			case ARRAYS:  // `c[a] = b`
				b = pop();
				a = pop();
				c = pop();

				if (a.type.id != TYPE_INT) {
					ierr("Index must be an int.");
				}

				if (a.v.v_int < 0) {
					ierr("Index must be more than 0.");
				}

				if (c.type.id != TYPE_ARRAY) {
					ierr("The object referenced is not an array.");
				}

				arr = c.v.v_array;

				if (a.v.v_int >= arr.len) {
					ierr("Index must be less than the length.");
				}

				if (!typeInfoEqual(b.type, c.type.args[0])) {
					ierr("Set type doens't match expression.");
				}

				arr.arr[a.v.v_int] = b.v;

				break;
			case ARRAYG:  // `a[b]`
				b = pop();
				a = pop();

				if (b.type.id != TYPE_INT) {
					ierr("Index must be an int.");
				}

				if (b.v.v_int < 0) {
					ierr("Index must be more than 0.");
				}

				if (a.type.id != TYPE_ARRAY) {
					ierr("The object referenced is not an array.");
				}

				arr = a.v.v_array;

				if (b.v.v_int >= arr.len) {
					ierr("Index must be less than the length.");
				}

				push((Object){
					.type = dupTypeInfo(a.type.args[0]),
					.v = arr.arr[b.v.v_int],
					.referenceId = a.referenceId,
				});

				break;
			case ARRAYL:  // `a.length`
				a = pop();

				if (a.type.id != TYPE_ARRAY) {
					ierr("The object referenced is not an array.");
				}

				arr = a.v.v_array;

				push((Object){
					.type = type(TYPE_INT),
					.v.v_int = arr.len,
				});

				break;
			case SWAP:
				b = stack[stackCount - 1];
				a = stack[stackCount - 2];

				stack[stackCount - 2] = b;
				stack[stackCount - 1] = a;

				// We don't want this to get freed
				a = (Object){0};
				b = (Object){0};

				break;
			case NOT:
				a = pop();

				if (a.type.id == TYPE_BOOL) {
					push((Object){.type = type(TYPE_BOOL), .v.v_int = !a.v.v_int});
				} else {
					ierr("Invalid type for `not`.");
				}

				break;
			case AND:
				b = pop();
				a = pop();

				if (a.type.id == TYPE_BOOL && b.type.id == TYPE_BOOL) {
					push((Object){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int && b.v.v_int});
				} else {
					ierr("Invalid types for `and`.");
				}

				break;
			case OR:
				b = pop();
				a = pop();

				if (a.type.id == TYPE_BOOL && b.type.id == TYPE_BOOL) {
					push((Object){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int || b.v.v_int});
				} else {
					ierr("Invalid types for `and`.");
				}

				break;
			case ADD:
				b = pop();
				a = pop();

				if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {
					push((Object){.type = type(TYPE_INT), .v.v_int = a.v.v_int + b.v.v_int});
				} else if (a.type.id == TYPE_FLOAT && b.type.id == TYPE_FLOAT) {
					push((Object){.type = type(TYPE_FLOAT), .v.v_float = a.v.v_float + b.v.v_float});
				} else if (a.type.id == TYPE_LONG && b.type.id == TYPE_LONG) {
					push((Object){.type = type(TYPE_LONG), .v.v_long = a.v.v_long + b.v.v_long});
				} else if (a.type.id == TYPE_STR && b.type.id == TYPE_STR) {
					// Create string
					String s = (String){
						.len = a.v.v_string.len + b.v.v_string.len,
					};
					s.chars = malloc(s.len);

					// Combine the two strings
					memcpy(s.chars, a.v.v_string.chars, a.v.v_string.len);
					memcpy(s.chars + a.v.v_string.len, b.v.v_string.chars, b.v.v_string.len);

					// Push onto stack
					push((Object){
						.type = type(TYPE_STR),
						.v.v_string = s,
						.referenceId = basicReference,
					});
				} else {
					ierr("Invalid types for `add`.");
				}

				break;
			case SUB:
				basicOperation(-);
				break;
			case MUL:
				basicOperation(*);
				break;
			case DIV:
				basicOperation(/);
				break;
			case MOD:
				b = pop();
				a = pop();
				if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {
					push((Object){.type = type(TYPE_INT), .v.v_int = a.v.v_int % b.v.v_int});
				} else if (a.type.id == TYPE_LONG && b.type.id == TYPE_LONG) {
					push((Object){.type = type(TYPE_LONG), .v.v_long = a.v.v_long % b.v.v_long});
				} else {
					ierr("Invalid types for `%%`.");
				}

				break;
			case POW:  // `a ^ b`
				b = pop();
				a = pop();
				if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {
					int number = 1;

					if (b.v.v_int < 0) {
						number = 0;
					} else {
						for (int j = 0; j < b.v.v_int; j++) {
							number *= a.v.v_int;
						}
					}

					push((Object){.type = type(TYPE_INT), .v.v_int = number});
				} else if (a.type.id == TYPE_LONG && b.type.id == TYPE_LONG) {
					long number = 1;

					if (b.v.v_long < 0) {
						number = 0;
					} else {
						for (long j = 0; j < b.v.v_long; j++) {
							number *= a.v.v_long;
						}
					}

					push((Object){.type = type(TYPE_LONG), .v.v_long = number});
				} else if (a.type.id == TYPE_FLOAT && b.type.id == TYPE_FLOAT) {
					push((Object){.type = type(TYPE_FLOAT), .v.v_long = powf(a.v.v_float, b.v.v_float)});
				} else {
					ierr("Invalid types for `^`.");
				}

				break;
			case NEG:
				a = pop();
				if (a.type.id == TYPE_INT) {
					push((Object){.type = type(TYPE_INT), .v.v_int = -a.v.v_int});
				} else if (a.type.id == TYPE_FLOAT) {
					push((Object){.type = type(TYPE_FLOAT), .v.v_float = -a.v.v_float});
				} else if (a.type.id == TYPE_LONG) {
					push((Object){.type = type(TYPE_LONG), .v.v_long = -a.v.v_long});
				} else {
					ierr("Invalid types for `neg`");
				}

				break;
			case EQ:
				b = pop();
				a = pop();

				if (a.type.id == TYPE_INT && b.type.id == TYPE_INT ||
					a.type.id == TYPE_FUNC && b.type.id == TYPE_FUNC ||
					a.type.id == TYPE_BOOL && b.type.id == TYPE_BOOL) {
					push((Object){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int == b.v.v_int});
				} else if (a.type.id == TYPE_LONG && b.type.id == TYPE_LONG) {
					push((Object){.type = type(TYPE_BOOL), .v.v_int = a.v.v_long == b.v.v_long});
				} else if (a.type.id == TYPE_STR && a.type.id == TYPE_STR) {
					push((Object){
						.type = type(TYPE_BOOL),
						.v.v_int = stringEqual(a.v.v_string, b.v.v_string),
					});
				} else {
					ierr("Invalid types for `eq`.");
				}

				break;
			case GT:
				boolOperation(>);
				break;
			case LT:
				boolOperation(<);
				break;
			case GTE:
				boolOperation(>=);
				break;
			case LTE:
				boolOperation(<=);
				break;
			case CAST:
				a = pop();

				if (insts[i].type.id == TYPE_STR) {
					if (a.type.id == TYPE_INT) {
						toStr(a.v.v_int, "%d");
					} else if (a.type.id == TYPE_FLOAT) {
						toStr(a.v.v_float, "%.9g");
					} else if (a.type.id == TYPE_LONG) {
						toStr(a.v.v_long, "%ld");
					} else if (a.type.id == TYPE_BOOL) {
						if (a.v.v_int) {
							push((Object){
								.type = type(TYPE_STR),
								.v.v_string = cstrToStr("true"),
								.referenceId = basicReference,
							});
						} else {
							push((Object){
								.type = type(TYPE_STR),
								.v.v_string = cstrToStr("false"),
								.referenceId = basicReference,
							});
						}
					} else {
						printf("Cannot cast type %d.\n", a.type.id);
						ierr("Invalid type for `cast` to `string`.");
					}
				} else if (insts[i].type.id == TYPE_INT) {
					if (a.type.id == TYPE_LONG) {
						push((Object){
							.type = type(TYPE_INT),
							.v.v_int = (int) a.v.v_long,
						});
					} else {
						printf("Cannot cast type %d.\n", a.type.id);
						ierr("Invalid type for `cast` to `int`.");
					}
				} else if (insts[i].type.id == TYPE_LONG) {
					if (a.type.id == TYPE_INT) {
						push((Object){
							.type = type(TYPE_LONG),
							.v.v_long = (long) a.v.v_int,
						});
					} else {
						printf("Cannot cast type %d.\n", a.type.id);
						ierr("Invalid type for `cast` to `long`.");
					}
				}

				break;
			case GOTO:
				jump(insts[i].a.v_int);
				break;
			case IFN:
				a = pop();

				if (a.type.id != TYPE_BOOL) {
					ierr("Invalid type for `ifn`.");
				}

				if (a.v.v_int == 0) {
					jump(insts[i].a.v_int);
				}

				break;
			case THROW:
				fprintf(stderr, "%s\n", (char*) insts[i].a.v_ptr);
				ierr("An error has been thrown, and execution has been aborted.");

				return;
			default:
				break;
		}

		if (a.type.args != NULL) {
			freeTypeInfo(a.type);
		}

		if (b.type.args != NULL) {
			freeTypeInfo(b.type);
		}

		if (c.type.args != NULL) {
			freeTypeInfo(c.type);
		}
	}
}

void bc_run(bool showByteCode, bool _showCount, bool _showDisposeInfo) {
	if (showByteCode) {
		bc_dump();
		return;
	}

	// Set flags

	showCount = _showCount;
	showDisposeInfo = _showDisposeInfo;

	// Read byte code

	ObjectList o;
	o.vars = malloc(0);
	o.varsCount = 0;
	pushFrame((CallFrame){.o = &o});

	readByteCode(framesCount - 1, 0);

	popFrame();
	disposeObjectList(&o);
}

void bc_end() {
	// Check for stack leaks

	if (stackCount != 0) {
		fprintf(stderr, "Stack Error: Stack leak detected. Ending size `%ld`.\n", stackCount);
	}

	if (locstackCount != 0) {
		fprintf(stderr, "Stack Error: Location stack leak detected. Ending size `%ld`.\n", locstackCount);
	}

	if (framesCount != 0) {
		fprintf(stderr, "Stack Error: CallFrame stack leak detected. Ending size `%ld`.\n", framesCount);
	}

	// Check for reference leaks

	for (size_t i = 0; i < refsCount; i++) {
		if (refs[i].counter > 0) {
			// fprintf(stderr, "Reference Warning: Reference `%ld` was not freed. It will be leaked.\n", i);
		}
	}

	// Free instructions

	for (size_t i = 0; i < instsCount; i++) {
		if (insts[i].type.args != NULL) {
			freeTypeInfo(insts[i].type);
		}

		// TODO: Free strings
	}

	// free(insts);

	// End

	free(funcs);
	// malloc_stats();
}