#pragma once

#include <string>
#include <thread>
#include <map>

#include <boost/process.hpp>
#include <boost/property_tree/ptree.hpp>

#include "json.hpp"
using json = nlohmann::json;

using namespace std;

class PIVID {

public:
	PIVID();
	~PIVID();

	void startServer(string mediaDirectory);

	boost::property_tree::ptree sendRequest(string target);
	boost::property_tree::ptree getFileMeta(string filename);
	float getDuration(string filename);

private:
	boost::process::child pividProcess;
	map<string, float> duration;
};