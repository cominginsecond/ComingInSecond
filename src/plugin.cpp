#include "plugin.hpp"


Plugin* pluginInstance;


void init(Plugin* p) {
	pluginInstance = p;

	// Add modules here
	p->addModel(modelQuantizerSequencer4);
	p->addModel(modelQuantizerSequencer);
	p->addModel(modelQuantizerSequencer16);
	p->addModel(modelVibraLFO);
	p->addModel(modelBlooth);
}
