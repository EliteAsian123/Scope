#include "types.h"

static Data createDefaultArray(const TypeInfo type) {
	Array a = (Array){
		.arr = NULL,
		.len = 0,
	};

	return (Data){._array = a};
}

static Data arrayDuplicate(const TypeInfo type, const Data v) {
	Data out = v;

	int size = sizeof(Value*) * out._array.len;
	out._array.arr = malloc(size);
	for (size_t i = 0; i < out._array.len; i++) {
		Value* val = malloc(sizeof(Value));
		*val = dupValue(*v._array.arr[i]);
		out._array.arr[i] = val;
	}

	return out;
}

static void disposeArray(const TypeInfo type, const Data v) {
	// if (isDisposable(type.args[0].id)) {
	// 	for (int i = 0; i < v._array.len; i++) {
	// 		refs[v._array.arr[i].referenceId].counter--;
	// 		disposeIfNoRefs(v._array.arr[i]);
	// 	}
	// }

	// free(v._array.arr);
}