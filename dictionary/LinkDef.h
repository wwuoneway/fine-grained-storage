#ifdef __ROOTCLING__
#pragma link off all globals;
#pragma link off all classes;
#pragma link off all functions;

#pragma link C++ class fgs::Position+;
#pragma link C++ class fgs::Momentum+;
#pragma link C++ class fgs::Token+;
#pragma link C++ class std::vector<fgs::Position>+;
#pragma link C++ class std::vector<fgs::Momentum>+;
#pragma link C++ class std::map<std::string,fgs::Token>+;
#endif
