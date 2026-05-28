# d

![Sun God](./assets/sun-god.png)

---

A dotfile manager.

## Features

- NOT "SUCKLESS" (IF YOURE'RE TRYING TO CONVINCE ME THAT YOUR SOFTWARE "SUCKS LESS",
  THEN IT ACTUALLY SUCKS!)
- NO "CONFIGURATION FILES" (THE CONCEPT OF "CONFIGURATION FILES" SHOULD NOT
  EXIST!)
- NO "DOCUMENTATION" (WHAT IS THAT?)
- NOT WRITTEN IN RUST (NO, I'M NOT INSANE! I LOVE RUST!)

## Summary

On a more serious note, `d` is your standard dotfile manager, with the twist that it can be configured using C, hopefully leveraging the ~~cursed~~ amazing C
preprocessor.

### Usage

```bash
git clone git@github.com:hyperupcall-projects/d
cd ./d
./bake build ~/.dotfiles/config/dotfiles.c
./bake install ~/.local
```

Your should have a `dotfiles.c` that looks something like:

```c
typedef enum {
	TYPE_ENTRY = 0,
	TYPE_GROUP = 1,
} ItemType;
typedef struct Item {
	int type;
	// Entry.
	char const *category;
	char const *source;
	char const *destination;
	// Group.
	struct Item **entries;
} Item;
typedef struct Deployment {
	char const *name;
	Item **items;
} Deployment;
#define Done { .type = TYPE_ENTRY, .source = NULL, .destination = NULL }
#define Home CONFIG_HOME "/"

// Each program has one or more entries.
static Item bash[] = {
	{
		.type = TYPE_ENTRY,
		.source = Home ".dotfiles/.bashrc",
		.destination = Home "/.bashrc"
	},
	{
		.type = TYPE_ENTRY,
		.source = Home ".dotfiles/.bash_login",
		.destination = Home ".bash_login"
	},
	Done
};
static Item zsh[] = {
	{
		.type = TYPE_ENTRY,
		.source = Home ".dotfiles/.zshrc",
		.destination = Home ".zshrc"
	},
	Done
};

// Each entry can be grouped into a single deployable unit.
static Item shellGroup = {
	.type = TYPE_GROUP,
	.entries = (Item *[]){
		bash,
		zsh,
		NULL
	}
};

// List all items for each deployment.
static Deployment deployment1 = {
	.name = "Linux desktop",
	.items = (Item *[]){
		&shellGroup,
		NULL
	}
};
static Deployment deployment2 = {
	.name = "Linux server",
	.items = (Item *[]){
		// Linux server should not deploy any shell dotfiles.
		NULL
	}
};

// List all possible deployments and configure a default.
static Deployment **deployments = (Deployment *[]){
	&deployment1,
	&deployment2,
	NULL
};
Deployment **getDeployments() {
	return deployments;
}
Deployment *getDefaultDeployment() {
	return &deployment2;
}
```

In summary, each item corresponds to some application and can have multiple dotfile files or directories. Items can be grouped together using a group item, or listed directly under a deployment. You must write `getDeployments()` and `getDefaultDeployment()` so `d` can see and use the deployments that you have.

The really cool part about this, is that you can use macros! This is your chance to be creative! See
[my dotfiles.c](https://github.com/hyperupcall/dotfiles/blob/trunk/config/dotfiles.c) for inspiration.

Now, you can use `d` like any other dotfile manager:

```console
$ d deploy
$ d undeploy
$ d print
```
