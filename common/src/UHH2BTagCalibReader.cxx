#include <UHH2/common/include/UHH2BTagCalibReader.h>

#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>

#include <TString.h>
#include <TObjArray.h>
#include <TObjString.h>

#define TOKEN_ELEMENT(IDX) ((TObjString*)tokens->At((IDX)))->GetString()

using namespace std;
using namespace uhh2::BTagCalib;

namespace uhh2 {

BTagCalib::Reader::Reader(
  const string & csvFilePath,
  const OperatingPoint op,
  const string & measurementType,
  const boost::optional<set<string> &> sysTypes,
  const boost::optional<bool> absEta,
  const boost::optional<bool> verbose,
  const boost::optional<float> defaultScaleFactor,
  const boost::optional<bool> failIfNoEntry
):
  fCSVFilePath(csvFilePath),
  fOperatingPoint(op),
  fMeasurementType(measurementType),
  bAbsEta(absEta ? *absEta : true),
  bVerbose(verbose ? *verbose : true),
  kDefaultScaleFactor(defaultScaleFactor ? *defaultScaleFactor : 1.f),
  bFailIfNoEntry(failIfNoEntry ? *failIfNoEntry : true),
  bShape(fOperatingPoint == OperatingPoint::SHAPE)
{
  if (bVerbose) cout << "Hello World from BTagCalib::Reader!"
  << "\nCSV: " << fCSVFilePath
  << "\nWorking point: " << kOperatingPoints.at(fOperatingPoint).csv_string
  << "\nMeasurement type: " << fMeasurementType
  << "\nUse absolute pseudo-rapidity: " << (bAbsEta ? "true" : "false") << endl;
  if (sysTypes) fSysTypes.insert(sysTypes->begin(), sysTypes->end());
  ReadCSV();
  if (bVerbose) cout << "BTagCalib::Reader has been initialized!" << endl;
}

void BTagCalib::Reader::ReadCSV()
{
  if (bVerbose) cout << "Reading CSV file ..." << endl;
  vector<TString> lines;
  string line;
  ifstream csvFile(fCSVFilePath);
  if (csvFile.is_open()) {
    while (getline(csvFile, line)) {
      if (line.empty()) continue; // skip empty lines
      else if (line.find("OperatingPoint") != string::npos) continue; // skip CSV header line
      else lines.push_back(TString(line));
    }
    csvFile.close();
  }
  else throw runtime_error("BTagCalib::Reader::ReadCSV(): Unable to open file");

  if (bVerbose) cout << "Done reading. Constructing hash map with calibrations ..." << endl;
  fSymbolTable.add_variable("x", fVariable); // "x" is the name of the variable in the formulas given in the CSV files
  fSymbolTable.add_constants();
  const int default_last = 10;
  unsigned long entries = 0;
  for (const TString & line : lines) {
    const TObjArray *tokens = line.Tokenize(",");
    const int last = tokens->GetLast();
    TString formula = TOKEN_ELEMENT(last);
    if (last > default_last) { // if number of commas in line is not 10 (i.e. 11 tokens); happens if the formula itself contains a commma
      formula = "";
      for (int i = default_last; i <= last; ++i) {
        formula += TOKEN_ELEMENT(i);
      }
    }
    else if (last < default_last) {
      stringstream ss;
      ss << "BTagCalib::Reader::ReadCSV(): File contains entry that does not have required format:\n" << line.Data();
      throw runtime_error(ss.str());
    }
    formula.ReplaceAll("\"", ""); // remove quotes that appear around some formulas; happens if they contain additional commas

    const string sysType = TOKEN_ELEMENT(2).Data();
    if (
      TOKEN_ELEMENT(0) != kOperatingPoints.at(fOperatingPoint).csv_string ||
      TOKEN_ELEMENT(1) != fMeasurementType ||
      fSysTypes.find(sysType) == fSysTypes.end()
    ) continue;

    const string expression_string = string(formula.Data());
    // expression_string must represent valid C code; problems could arise e.g. if it includes ROOT-internal stuff like "TMath::Power" instead of "pow".
    // Right now, the CSV files seem to consistently use the basic math functions/operators (e.g. +, -, *, /, pow, exp, ...) that can be interpreted by
    // the exprtk library. In case BTV ever changes that, we would need to do more pre-processing of the formula here.
    CalibEntry entry = {
      .etaMin = float(TOKEN_ELEMENT(4).Atof()),
      .etaMax = float(TOKEN_ELEMENT(5).Atof()),
      .ptMin = float(TOKEN_ELEMENT(6).Atof()),
      .ptMax = float(TOKEN_ELEMENT(7).Atof()),
      .discrMin = float(TOKEN_ELEMENT(8).Atof()),
      .discrMax = float(TOKEN_ELEMENT(9).Atof()),
      .func = make_shared<expression_t>()
    };
    entry.func->register_symbol_table(fSymbolTable);
    if (!fParser.compile(expression_string, *entry.func)) {
      stringstream ss;
      ss << "BTagCalib::Reader::ReadCSV(): Compilation error for formula:\n" << expression_string;
      throw runtime_error(ss.str());
    }

    JetFlavor jf = JetFlavor::UNDEFINED;
    const string jf_str = TOKEN_ELEMENT(3).Data();
    for (const auto & kjf : kJetFlavors) {
      if (kjf.second.csv_string == jf_str) {
        jf = kjf.first;
        break;
      }
    }
    if (jf == JetFlavor::UNDEFINED) throw runtime_error("BTagCalib::Reader::ReadCSV(): File contains unknown flavor type: "+jf_str);

    fCalibEntries[sysType][jf].push_back(entry);
    entries++;
  }
  if (bVerbose) cout << "Hash map constructed. Number of entries: "+to_string(entries) << endl;
}

float BTagCalib::Reader::Evaluate(
  const std::string & sysType,
  const JetFlavor jf,
  float eta,
  float pt,
  float discr
)
{
  if (!fCalibEntries.count(sysType)) throw runtime_error("BTagCalib::Reader::Evaluate(): No entries for sysType '"+sysType+"' found. Not loaded?");
  else if (!fCalibEntries.at(sysType).count(jf)) {
    stringstream ss;
    ss << "BTagCalib::Reader::Evaluate(): No entries for jetFlavor '"+kJetFlavors.at(jf).csv_string+"' found for sysType '"+sysType+"'.\n"
    << "This (probably) means that you request a scale factor for a particular sysType that is not provided for this jetFlavor at all.";
    throw runtime_error(ss.str());
  }

  const auto & entries = fCalibEntries.at(sysType).at(jf);
  bool out_of_bounds = false;
  if (bAbsEta && eta < 0) eta = -eta;

  // Restrict eta to boundaries
  float etaMin = F_MAX;
  float etaMax = F_MIN;
  for (const auto & entry : entries) {
    etaMin = min(etaMin, entry.etaMin);
    etaMax = max(etaMax, entry.etaMax);
  }
  if (eta < etaMin || eta > etaMax) out_of_bounds = true;
  eta = min(max(eta, nextafterf(etaMin, F_MAX)), nextafterf(etaMax, F_MIN));

  // Restrict pt to boundaries of this eta bin
  float ptMin = F_MAX;
  float ptMax = F_MIN;
  for (const auto & entry : entries) {
    if (
      entry.etaMin <= eta && eta <= entry.etaMax
    ) {
      ptMin = min(ptMin, entry.ptMin);
      ptMax = max(ptMax, entry.ptMax);
    }
  }
  if (pt < ptMin || pt > ptMax) out_of_bounds = true;
  pt = min(max(pt, nextafterf(ptMin, F_MAX)), nextafterf(ptMax, F_MIN));

  if (bShape) {
    // Restrict discr to boundaries of this eta/pt bin
    float discrMin = F_MAX;
    float discrMax = F_MIN;
    for (const auto & entry : entries) {
      if (
        entry.etaMin <= eta && eta <= entry.etaMax
        && entry.ptMin <= pt && pt <= entry.ptMax
      ) {
        discrMin = min(discrMin, entry.discrMin);
        discrMax = max(discrMax, entry.discrMax);
      }
    }
    // Don't need an out-of-bounds check here because CSV files for the reshaping method contain an underflow and overflow bin for discr
    discr = min(max(discr, nextafterf(discrMin, F_MAX)), nextafterf(discrMax, F_MIN));
  }

  float sf = kDefaultScaleFactor;
  bool entry_found = false;
  for (const auto & entry : entries) {
    if (
      entry.etaMin <= eta && eta <= entry.etaMax
      && entry.ptMin <= pt && pt <= entry.ptMax
      && (bShape ? (entry.discrMin <= discr && discr <= entry.discrMax) : true)
    ) {
      fVariable = bShape ? discr : pt;
      sf = entry.func->value();
      entry_found = true;
      break;
    }
  }

  if (!entry_found) {
    stringstream ss;
    ss << "BTagEntry::Evaluate(): No valid calibration entry can be found for this sysType/jetFlavor/eta/pt/discr combination:\n"
    << sysType << " / " << kJetFlavors.at(jf).csv_string << " / " << to_string(eta) << " / " << to_string(pt) << " / " << to_string(discr);
    if (bFailIfNoEntry) throw runtime_error(ss.str());
    else if (bVerbose) cout << ss.str() << "\nScale factor evaluated to default value "+to_string(sf) << endl;
  }
  else if (sysType != CENTRAL_CSV_STRING && out_of_bounds) {
    const float sf_central = Evaluate(CENTRAL_CSV_STRING, jf, eta, pt, discr);
    sf = sf_central + 2*(sf - sf_central); // double uncertainty if out of eta/pt bounds
  }
  return sf;
}

}
