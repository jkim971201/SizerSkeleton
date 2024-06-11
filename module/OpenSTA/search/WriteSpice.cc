// OpenSTA, Static Timing Analyzer
// Copyright (c) 2024, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "WriteSpice.hh"

#include "Debug.hh"
#include "Units.hh"
#include "TableModel.hh"
#include "TimingRole.hh"
#include "FuncExpr.hh"
#include "Sequential.hh"
#include "PortDirection.hh"
#include "TimingArc.hh"
#include "Liberty.hh"
#include "Network.hh"
#include "Graph.hh"
#include "Sim.hh"
#include "Clock.hh"
#include "PathVertex.hh"
#include "DcalcAnalysisPt.hh"
#include "Bdd.hh"

namespace sta {

using std::ifstream;

Net *
pinNet(const Pin *pin,
       const Network *network);

class SubcktEndsMissing : public Exception
{
public:
  SubcktEndsMissing(const char *cell_name,
		    const char *subckt_filename);
  const char *what() const noexcept;

protected:
  string what_;
};

SubcktEndsMissing::SubcktEndsMissing(const char *cell_name,
				     const char *subckt_filename) :
  Exception()
{
  what_ = "spice subckt for cell ";
  what_ += cell_name;
  what_ += " missing .ends in ";
  what_ += subckt_filename;
}

const char *
SubcktEndsMissing::what() const noexcept
{
  return what_.c_str();
}

////////////////////////////////////////////////////////////////

WriteSpice::WriteSpice(const char *spice_filename,
                       const char *subckt_filename,
                       const char *lib_subckt_filename,
                       const char *model_filename,
                       const char *power_name,
                       const char *gnd_name,
                       CircuitSim ckt_sim,
                       const StaState *sta) :
  StaState(sta),
  spice_filename_(spice_filename),
  subckt_filename_(subckt_filename),
  lib_subckt_filename_(lib_subckt_filename),
  model_filename_(model_filename),
  power_name_(power_name),
  gnd_name_(gnd_name),
  ckt_sim_(ckt_sim),
  default_library_(network_->defaultLibertyLibrary()),
  short_ckt_resistance_(.0001),
  cap_index_(1),
  res_index_(1),
  volt_index_(1),
  next_node_index_(1),
  bdd_(sta)
{
}

void
WriteSpice::initPowerGnd(const DcalcAnalysisPt *dcalc_ap)
{
  bool exists = false;
  default_library_->supplyVoltage(power_name_, power_voltage_, exists);
  if (!exists) {
    const OperatingConditions *op_cond = dcalc_ap->operatingConditions();
    if (op_cond == nullptr)
      op_cond = network_->defaultLibertyLibrary()->defaultOperatingConditions();
    power_voltage_ = op_cond->voltage();
  }
  default_library_->supplyVoltage(gnd_name_, gnd_voltage_, exists);
  if (!exists)
    gnd_voltage_ = 0.0;
}

// Use c++17 fs::path(filename).stem()
static string
filenameStem(const char *filename)
{
  string filename1 = filename;
  const size_t last_slash_idx = filename1.find_last_of("\\/");
  if (last_slash_idx != std::string::npos)
    return filename1.substr(last_slash_idx + 1);
  else
    return filename1;
}

void
WriteSpice::writeHeader(string &title,
                        float max_time,
                        float time_step)
{
  streamPrint(spice_stream_, "* %s\n", title.c_str());
  streamPrint(spice_stream_, ".include \"%s\"\n", model_filename_);
  string subckt_filename_stem = filenameStem(subckt_filename_);
  streamPrint(spice_stream_, ".include \"%s\"\n", subckt_filename_stem.c_str());

  streamPrint(spice_stream_, ".tran %.3g %.3g\n", time_step, max_time);
  // Suppress printing model parameters.
  if (ckt_sim_ == CircuitSim::hspice)
    streamPrint(spice_stream_, ".options nomod\n");
  streamPrint(spice_stream_, "\n");
  max_time_ = max_time;
}

void
WriteSpice::writePrintStmt(StdStringSeq &node_names)
{
  streamPrint(spice_stream_, ".print tran");
  if (ckt_sim_ == CircuitSim::xyce) {
    string csv_filename = replaceFileExt(spice_filename_, "csv");
    streamPrint(spice_stream_, " format=csv file=%s", csv_filename.c_str());
    writeGnuplotFile(node_names);
  }
  for (string &name : node_names)
    streamPrint(spice_stream_, " v(%s)", name.c_str());
  streamPrint(spice_stream_, "\n\n");
}

string
WriteSpice::replaceFileExt(string filename,
                           const char *ext)
{
  size_t dot = filename.rfind('.');
  string ext_filename = filename.substr(0, dot + 1);
  ext_filename += ext;
  return ext_filename;
}

// Write gnuplot command file for use with xyce csv file.
void
WriteSpice::writeGnuplotFile(StdStringSeq &node_nanes)
{
  string gnuplot_filename = replaceFileExt(spice_filename_, "gnuplot");
  string csv_filename = replaceFileExt(spice_filename_, "csv");
  ofstream gnuplot_stream;
  gnuplot_stream.open(gnuplot_filename);
  if (gnuplot_stream.is_open()) {
    streamPrint(gnuplot_stream, "set datafile separator ','\n");
    streamPrint(gnuplot_stream, "set key autotitle columnhead\n");
    streamPrint(gnuplot_stream, "plot\\\n");
    streamPrint(gnuplot_stream, "\"%s\" using 1:2 with lines",
                csv_filename.c_str());
    for (size_t i = 3; i <= node_nanes.size() + 1; i++) {
      streamPrint(gnuplot_stream, ",\\\n");
      streamPrint(gnuplot_stream, "'' using 1:%lu with lines", i);
    }
    streamPrint(gnuplot_stream, "\n");
    streamPrint(gnuplot_stream, "pause mouse close\n");
    gnuplot_stream.close();
  }
}

void
WriteSpice::writeSubckts(StdStringSet &cell_names)
{
  findCellSubckts(cell_names);
  ifstream lib_subckts_stream(lib_subckt_filename_);
  if (lib_subckts_stream.is_open()) {
    ofstream subckts_stream(subckt_filename_);
    if (subckts_stream.is_open()) {
      string line;
      while (getline(lib_subckts_stream, line)) {
	// .subckt <cell_name> [args..]
	StringVector tokens;
	split(line, " \t", tokens);
	if (tokens.size() >= 2
	    && stringEqual(tokens[0].c_str(), ".subckt")) {
	  const char *cell_name = tokens[1].c_str();
	  if (cell_names.find(cell_name) != cell_names.end()) {
	    subckts_stream << line << "\n";
	    bool found_ends = false;
	    while (getline(lib_subckts_stream, line)) {
	      subckts_stream << line << "\n";
	      if (stringBeginEqual(line.c_str(), ".ends")) {
		subckts_stream << "\n";
		found_ends = true;
		break;
	      }
	    }
	    if (!found_ends)
	      throw SubcktEndsMissing(cell_name, lib_subckt_filename_);
	    cell_names.erase(cell_name);
	  }
	  recordSpicePortNames(cell_name, tokens);
	}
      }
      subckts_stream.close();
      lib_subckts_stream.close();

      if (!cell_names.empty()) {
	string missing_cells;
        for (const string &cell_name : cell_names) {
	  missing_cells += "\n";
	  missing_cells += cell_name;
        }
	report_->error(1605, "The subkct file %s is missing definitions for %s",
		       lib_subckt_filename_,
                       missing_cells.c_str());
      }
    }
    else {
      lib_subckts_stream.close();
      throw FileNotWritable(subckt_filename_);
    }
  }
  else
    throw FileNotReadable(lib_subckt_filename_);
}

void
WriteSpice::recordSpicePortNames(const char *cell_name,
                                 StringVector &tokens)
{
  LibertyCell *cell = network_->findLibertyCell(cell_name);
  if (cell) {
    StringVector &spice_port_names = cell_spice_port_names_[cell_name];
    for (size_t i = 2; i < tokens.size(); i++) {
      const char *port_name = tokens[i].c_str();
      LibertyPort *port = cell->findLibertyPort(port_name);
      LibertyPgPort *pg_port = cell->findPgPort(port_name);
      if (port == nullptr
	  && pg_port == nullptr
	  && !stringEqual(port_name, power_name_)
	  && !stringEqual(port_name, gnd_name_))
	report_->error(1606, "subckt %s port %s has no corresponding liberty port, pg_port and is not power or ground.",
		       cell_name, port_name);
      spice_port_names.push_back(port_name);
    }
  }
}

// Subckts can call subckts (asap7).
void
WriteSpice::findCellSubckts(StdStringSet &cell_names)
{
  ifstream lib_subckts_stream(lib_subckt_filename_);
  if (lib_subckts_stream.is_open()) {
    string line;
    while (getline(lib_subckts_stream, line)) {
      // .subckt <cell_name> [args..]
      StringVector tokens;
      split(line, " \t", tokens);
      if (tokens.size() >= 2
          && stringEqual(tokens[0].c_str(), ".subckt")) {
        const char *cell_name = tokens[1].c_str();
        if (cell_names.find(cell_name) != cell_names.end()) {
          // Scan the subckt definition for subckt calls.
          string stmt;
          while (getline(lib_subckts_stream, line)) {
            if (line[0] == '+')
              stmt += line.substr(1);
            else {
              // Process previous statement.
              if (tolower(stmt[0]) == 'x') {
                split(stmt, " \t", tokens);
                string &subckt_cell = tokens[tokens.size() - 1];
                cell_names.insert(subckt_cell);
              }
              stmt = line;
            }
            if (stringBeginEqual(line.c_str(), ".ends"))
              break;
          }
        }
      }
    }
  }
  else
    throw FileNotReadable(lib_subckt_filename_);
}

////////////////////////////////////////////////////////////////

void
WriteSpice::writeSubcktInst(const Pin *input_pin)
{
  const Instance *inst = network_->instance(input_pin);
  const char *inst_name = network_->pathName(inst);
  LibertyCell *cell = network_->libertyCell(inst);
  const char *cell_name = cell->name();
  StringVector &spice_port_names = cell_spice_port_names_[cell_name];
  streamPrint(spice_stream_, "x%s", inst_name);
  for (string subckt_port_name : spice_port_names) {
    const char *subckt_port_cname = subckt_port_name.c_str();
    Pin *pin = network_->findPin(inst, subckt_port_cname);
    LibertyPgPort *pg_port = cell->findPgPort(subckt_port_cname);
    const char *pin_name;
    if (pin) {
      pin_name = network_->pathName(pin);
      streamPrint(spice_stream_, " %s", pin_name);
    }
    else if (pg_port)
      streamPrint(spice_stream_, " %s/%s", inst_name, subckt_port_cname);
    else if (stringEq(subckt_port_cname, power_name_)
	     || stringEq(subckt_port_cname, gnd_name_))
      streamPrint(spice_stream_, " %s/%s", inst_name, subckt_port_cname);
  }
  streamPrint(spice_stream_, " %s\n", cell_name);
}

// Power/ground and input voltage sources.
void
WriteSpice::writeSubcktInstVoltSrcs(const Pin *input_pin,
                                    LibertyPortLogicValues &port_values,
                                    const Clock *clk,
                                    DcalcAPIndex dcalc_ap_index)
{
  const Instance *inst = network_->instance(input_pin);
  LibertyCell *cell = network_->libertyCell(inst);
  const char *cell_name = cell->name();
  StringVector &spice_port_names = cell_spice_port_names_[cell_name];

  const LibertyPort *input_port = network_->libertyPort(input_pin);
  const char *inst_name = network_->pathName(inst);

  debugPrint(debug_, "write_spice", 2, "subckt %s", cell->name());
  for (string subckt_port_sname : spice_port_names) {
    const char *subckt_port_name = subckt_port_sname.c_str();
    LibertyPort *port = cell->findLibertyPort(subckt_port_name);
    LibertyPgPort *pg_port = cell->findPgPort(subckt_port_name);
    debugPrint(debug_, "write_spice", 2, " port %s%s",
               subckt_port_name,
               pg_port ? " pwr/gnd" : "");
    if (pg_port)
      writeVoltageSource(inst_name, subckt_port_name,
			 pgPortVoltage(pg_port));
    else if (stringEq(subckt_port_name, power_name_))
      writeVoltageSource(inst_name, subckt_port_name, power_voltage_);
    else if (stringEq(subckt_port_name, gnd_name_))
      writeVoltageSource(inst_name, subckt_port_name, gnd_voltage_);
    else if (port
             && port != input_port
             && port->direction()->isAnyInput()) {
      // Input voltage to sensitize path from gate input to output.
      const Pin *pin = network_->findPin(inst, port);
      // Look for tie high/low or propagated constant values.
      LogicValue port_value = sim_->logicValue(pin);
      if (port_value == LogicValue::unknown) {
        bool has_value;
        LogicValue value;
        port_values.findKey(port, value, has_value);
        if (has_value)
          port_value = value;
      }
      switch (port_value) {
      case LogicValue::zero:
      case LogicValue::unknown:
        writeVoltageSource(cell, inst_name, subckt_port_name,
                           port->relatedGroundPin(),
                           gnd_voltage_);
        break;
      case LogicValue::one:
        writeVoltageSource(cell, inst_name, subckt_port_name,
                           port->relatedPowerPin(),
                           power_voltage_);
        break;
      case LogicValue::rise:
        writeClkedStepSource(pin, RiseFall::rise(), clk, dcalc_ap_index);

        break;
      case LogicValue::fall:
        writeClkedStepSource(pin, RiseFall::fall(), clk, dcalc_ap_index);
        break;
      }
    }
  }
}

void
WriteSpice::writeVoltageSource(const char *inst_name,
                               const char *port_name,
                               float voltage)
{
  string node_name = inst_name;
  node_name += '/';
  node_name += port_name;
  writeVoltageSource(node_name.c_str(), voltage);
}

void
WriteSpice::writeVoltageSource(LibertyCell *cell,
                               const char *inst_name,
                               const char *subckt_port_name,
                               const char *pg_port_name,
                               float voltage)
{
  if (pg_port_name) {
    LibertyPgPort *pg_port = cell->findPgPort(pg_port_name);
    if (pg_port)
      voltage = pgPortVoltage(pg_port);
    else
      report_->error(1603, "%s pg_port %s not found,",
		     cell->name(),
		     pg_port_name);

  }
  writeVoltageSource(inst_name, subckt_port_name, voltage);
}

float
WriteSpice::pgPortVoltage(LibertyPgPort *pg_port)
{
  LibertyLibrary *liberty = pg_port->cell()->libertyLibrary();
  float voltage = 0.0;
  bool exists;
  const char *voltage_name = pg_port->voltageName();
  if (voltage_name) {
    liberty->supplyVoltage(voltage_name, voltage, exists);
    if (!exists) {
      if (stringEqual(voltage_name, power_name_))
	voltage = power_voltage_;
      else if (stringEqual(voltage_name, gnd_name_))
	voltage = gnd_voltage_;
      else
	report_->error(1601 , "pg_pin %s/%s voltage %s not found,",
		       pg_port->cell()->name(),
		       pg_port->name(),
		       voltage_name);
    }
  }
  else
    report_->error(1602, "Liberty pg_port %s/%s missing voltage_name attribute,",
		   pg_port->cell()->name(),
		   pg_port->name());
  return voltage;
}

// PWL voltage source that rises half way into the first clock cycle.
void
WriteSpice::writeClkedStepSource(const Pin *pin,
                                 const RiseFall *rf,
                                 const Clock *clk,
                                 DcalcAPIndex dcalc_ap_index)
{
  Vertex *vertex = graph_->pinLoadVertex(pin);
  float slew = findSlew(vertex, rf, nullptr, dcalc_ap_index);
  float time = clkWaveformTimeOffset(clk) + clk->period() / 2.0;
  writeRampVoltSource(pin, rf, time, slew);
}

float
WriteSpice::clkWaveformTimeOffset(const Clock *clk)
{
  return clk->period() / 10;
}

////////////////////////////////////////////////////////////////

float
WriteSpice::findSlew(Vertex *vertex,
                     const RiseFall *rf,
                     TimingArc *next_arc,
                     DcalcAPIndex dcalc_ap_index)
{
  float slew = delayAsFloat(graph_->slew(vertex, rf, dcalc_ap_index));
  if (slew == 0.0 && next_arc)
    slew = slewAxisMinValue(next_arc);
  if (slew == 0.0)
    slew = units_->timeUnit()->scale();
  return slew;
}

// Look up the smallest slew axis value in the timing arc delay table.
float
WriteSpice::slewAxisMinValue(TimingArc *arc)
{
  GateTableModel *gate_model = dynamic_cast<GateTableModel*>(arc->model());
  if (gate_model) {
    const TableModel *model = gate_model->delayModel();
    const TableAxis *axis1 = model->axis1();
    TableAxisVariable var1 = axis1->variable();
    if (var1 == TableAxisVariable::input_transition_time
	|| var1 == TableAxisVariable::input_net_transition)
      return axis1->axisValue(0);

    const TableAxis *axis2 = model->axis2();
    TableAxisVariable var2 = axis2->variable();
    if (var2 == TableAxisVariable::input_transition_time
	|| var2 == TableAxisVariable::input_net_transition)
      return axis2->axisValue(0);

    const TableAxis *axis3 = model->axis3();
    TableAxisVariable var3 = axis3->variable();
    if (var3 == TableAxisVariable::input_transition_time
	|| var3 == TableAxisVariable::input_net_transition)
      return axis3->axisValue(0);
  }
  return 0.0;
}

////////////////////////////////////////////////////////////////

void
WriteSpice::writeDrvrParasitics(const Pin *drvr_pin,
                                const RiseFall *drvr_rf,
                                const NetSet &aggressor_nets,
                                const ParasiticAnalysisPt *parasitic_ap)
{
  Net *net = network_->net(drvr_pin);
  const char *net_name = net ? network_->pathName(net) : network_->pathName(drvr_pin);
  streamPrint(spice_stream_, "* Net %s\n", net_name);

  Parasitic *parasitic = parasitics_->findParasiticNetwork(drvr_pin, parasitic_ap);
  node_map_.clear();
  next_node_index_ = 1;
  if (parasitic)
    writeParasiticNetwork(drvr_pin, parasitic, aggressor_nets);
  else {
    parasitic = parasitics_->findPiElmore(drvr_pin, drvr_rf, parasitic_ap);
    if (parasitic)
      writePiElmore(drvr_pin, parasitic);
    else {
      streamPrint(spice_stream_, "* No parasitics found for this net.\n");
      writeNullParasitic(drvr_pin);
    }
  }
}

void
WriteSpice::writeParasiticNetwork(const Pin *drvr_pin,
                                  const Parasitic *parasitic,
                                  const NetSet &coupling_nets)
{
  Set<const Pin*> reachable_pins;
  // Sort resistors for consistent regression results.
  ParasiticResistorSeq resistors = parasitics_->resistors(parasitic);
  sort(resistors.begin(), resistors.end(),
       [=] (const ParasiticResistor *r1,
            const ParasiticResistor *r2) {
         return parasitics_->id(r1) < parasitics_->id(r2);
       });
  for (ParasiticResistor *resistor : resistors) {
    float resistance = parasitics_->value(resistor);
    ParasiticNode *node1 = parasitics_->node1(resistor);
    ParasiticNode *node2 = parasitics_->node2(resistor);
    streamPrint(spice_stream_, "R%d %s %s %.3e\n",
                res_index_++,
                nodeName(node1),
                nodeName(node2),
                resistance);

    // Necessary but not sufficient. Need a DFS.
    const Pin *pin1 = parasitics_->pin(node1);
    if (pin1)
      reachable_pins.insert(pin1);
    const Pin *pin2 = parasitics_->pin(node2);
    if (pin2)
      reachable_pins.insert(pin2);
  }

  // Add resistors from drvr to load for missing parasitic connections.
  auto pin_iter = network_->connectedPinIterator(drvr_pin);
  while (pin_iter->hasNext()) {
    const Pin *pin = pin_iter->next();
    if (pin != drvr_pin
	&& network_->isLoad(pin)
	&& !network_->isHierarchical(pin)
	&& !reachable_pins.hasKey(pin)) {
      streamPrint(spice_stream_, "R%d %s %s %.3e\n",
		  res_index_++,
		  network_->pathName(drvr_pin),
		  network_->pathName(pin),
		  short_ckt_resistance_);
    }
  }
  delete pin_iter;

  // Sort coupling capacitors consistent regression results.
  ParasiticCapacitorSeq capacitors = parasitics_->capacitors(parasitic);
  sort(capacitors.begin(), capacitors.end(),
       [=] (const ParasiticCapacitor *c1,
            const ParasiticCapacitor *c2) {
         return parasitics_->id(c1) < parasitics_->id(c2);
       });
  const Net *net = pinNet(drvr_pin, network_);
  for (ParasiticCapacitor *capacitor : capacitors) {
    ParasiticNode *node1 = parasitics_->node1(capacitor);
    ParasiticNode *node2 = parasitics_->node2(capacitor);
    float cap = parasitics_->value(capacitor);
    const Net *net1 = node1 ? parasitics_->net(node1, network_) : nullptr;
    const Net *net2 = node2 ? parasitics_->net(node2, network_) : nullptr;
    const ParasiticNode *net_node = nullptr;
    const char *coupling_name;
    if (net1 == net) {
      net_node = node1;
      coupling_name = net2 && coupling_nets.hasKey(net2) ? nodeName(node2) : "0";
    }
    else if (net2 == net) {
      net_node = node2;
      coupling_name = net1 && coupling_nets.hasKey(net1) ? nodeName(node1) : "0";
    }
    if (net_node)
      streamPrint(spice_stream_, "C%d %s %s %.3e\n",
                  cap_index_++,
                  nodeName(net_node),
                  coupling_name,
                  cap);
  }

  // Sort nodes for consistent regression results.
  ParasiticNodeSeq nodes = parasitics_->nodes(parasitic);
  sort(nodes.begin(), nodes.end(),
       [=] (const ParasiticNode *node1,
            const ParasiticNode *node2) {
         const char *name1 = parasitics_->name(node1);
         const char *name2 = parasitics_->name(node2);
         return stringLess(name1, name2);
       });

  for (ParasiticNode *node : nodes) {
    float cap = parasitics_->nodeGndCap(node);
    // Spice has a cow over zero value caps.
    if (cap > 0.0) {
      streamPrint(spice_stream_, "C%d %s 0 %.3e\n",
                  cap_index_++,
                  nodeName(node),
                  cap);
    }
  }
}

Net *
pinNet(const Pin *pin,
       const Network *network)
{
  Net *net = network->net(pin);
  // Pins on the top level instance may not have nets.
  // Use the net connected to the pin's terminal.
  if (net == nullptr && network->isTopLevelPort(pin)) {
    Term *term = network->term(pin);
    if (term)
      return network->net(term);
  }
  return net;
}

const char *
WriteSpice::nodeName(const ParasiticNode *node)
{
  const Pin *pin = parasitics_->pin(node);
  if (pin)
    return parasitics_->name(node);
  else {
    int node_index;
    auto index_itr = node_map_.find(node);
    if (index_itr == node_map_.end()) {
      node_index = next_node_index_++;
      node_map_[node] = node_index;
    }
    else
      node_index = index_itr->second;
    const Net *net = parasitics_->net(node, network_);
    const char *net_name = network_->pathName(net);
    return stringPrintTmp("%s/%d", net_name, node_index);
  }
}

void
WriteSpice::writePiElmore(const Pin *drvr_pin,
                          const Parasitic *parasitic)
{
  float c2, rpi, c1;
  parasitics_->piModel(parasitic, c2, rpi, c1);
  const char *c1_node = "n1";
  streamPrint(spice_stream_, "RPI %s %s %.3e\n",
              network_->pathName(drvr_pin),
              c1_node,
              rpi);
  if (c2 > 0.0)
    streamPrint(spice_stream_, "C2 %s 0 %.3e\n",
                network_->pathName(drvr_pin),
                c2);
  if (c1 > 0.0)
    streamPrint(spice_stream_, "C1 %s 0 %.3e\n",
                c1_node,
                c1);
  
  int load_index = 3;
  auto pin_iter = network_->connectedPinIterator(drvr_pin);
  while (pin_iter->hasNext()) {
    const Pin *load_pin = pin_iter->next();
    if (load_pin != drvr_pin
	&& network_->isLoad(load_pin)
	&& !network_->isHierarchical(load_pin)) {
      float elmore;
      bool exists;
      parasitics_->findElmore(parasitic, load_pin, elmore, exists);
      if (exists) {
        streamPrint(spice_stream_, "E%d el%d 0 %s 0 1.0\n",
                    load_index,
                    load_index,
                    network_->pathName(drvr_pin));
        streamPrint(spice_stream_, "R%d el%d %s 1.0\n",
                    load_index,
                    load_index,
                    network_->pathName(load_pin));
        streamPrint(spice_stream_, "C%d %s 0 %.3e\n",
                    load_index,
                    network_->pathName(load_pin),
                    elmore);
      }
      else
        // Add resistor from drvr to load for missing elmore.
        streamPrint(spice_stream_, "R%d %s %s %.3e\n",
                    load_index,
                    network_->pathName(drvr_pin),
                    network_->pathName(load_pin),
                    short_ckt_resistance_);
      load_index++;
    }
  }
  delete pin_iter;
}

void
WriteSpice::writeNullParasitic(const Pin *drvr_pin)
{
  // Add resistors from drvr to load for missing parasitic connections.
  auto pin_iter = network_->connectedPinIterator(drvr_pin);
  while (pin_iter->hasNext()) {
    const Pin *load_pin = pin_iter->next();
    if (load_pin != drvr_pin
	&& network_->isLoad(load_pin)
	&& !network_->isHierarchical(load_pin)) {
      streamPrint(spice_stream_, "R%d %s %s %.3e\n",
                  res_index_++,
                  network_->pathName(drvr_pin),
                  network_->pathName(load_pin),
                  short_ckt_resistance_);
    }
  }
  delete pin_iter;
}

////////////////////////////////////////////////////////////////

void
WriteSpice::writeVoltageSource(const char *node_name,
                               float voltage)
{
  streamPrint(spice_stream_, "v%d %s 0 %.3f\n",
              volt_index_++,
              node_name,
              voltage);
}

void
WriteSpice::writeWaveformVoltSource(const Pin *pin,
                                    DriverWaveform *drvr_waveform,
                                    const RiseFall *rf,
                                    float delay,
                                    float slew)
{
  float volt0, volt1, volt_factor;
  if (rf == RiseFall::rise()) {
    volt0 = gnd_voltage_;
    volt1 = power_voltage_;
    volt_factor = power_voltage_;
  }
  else {
    volt0 = power_voltage_;
    volt1 = gnd_voltage_;
    volt_factor = -power_voltage_;
  }
  streamPrint(spice_stream_, "v%d %s 0 pwl(\n",
	      volt_index_++,
	      network_->pathName(pin));
  streamPrint(spice_stream_, "+%.3e %.3e\n", 0.0, volt0);
  Table1 waveform = drvr_waveform->waveform(slew);
  const TableAxis *time_axis = waveform.axis1();
  for (size_t time_index = 0; time_index <  time_axis->size(); time_index++) {
    float time = delay + time_axis->axisValue(time_index);
    float wave_volt = waveform.value(time_index);
    float volt = volt0 + wave_volt * volt_factor;
    streamPrint(spice_stream_, "+%.3e %.3e\n", time, volt);
  }
  streamPrint(spice_stream_, "+%.3e %.3e\n", max_time_, volt1);
  streamPrint(spice_stream_, "+)\n");
}

void
WriteSpice::writeRampVoltSource(const Pin *pin,
                                const RiseFall *rf,
                                float time,
                                float slew)
{
  float volt0, volt1;
  if (rf == RiseFall::rise()) {
    volt0 = gnd_voltage_;
    volt1 = power_voltage_;
  }
  else {
    volt0 = power_voltage_;
    volt1 = gnd_voltage_;
  }
  streamPrint(spice_stream_, "v%d %s 0 pwl(\n",
	      volt_index_++,
	      network_->pathName(pin));
  streamPrint(spice_stream_, "+%.3e %.3e\n", 0.0, volt0);
  writeWaveformEdge(rf, time, slew);
  streamPrint(spice_stream_, "+%.3e %.3e\n", max_time_, volt1);
  streamPrint(spice_stream_, "+)\n");
}

// Write PWL rise/fall edge that crosses threshold at time.
void
WriteSpice::writeWaveformEdge(const RiseFall *rf,
                              float time,
                              float slew)
{
  float volt0, volt1;
  if (rf == RiseFall::rise()) {
    volt0 = gnd_voltage_;
    volt1 = power_voltage_;
  }
  else {
    volt0 = power_voltage_;
    volt1 = gnd_voltage_;
  }
  float threshold = default_library_->inputThreshold(rf);
  float dt = railToRailSlew(slew, rf);
  float time0 = time - dt * threshold;
  float time1 = time0 + dt;
  if (time0 > 0.0)
    streamPrint(spice_stream_, "+%.3e %.3e\n", time0, volt0);
  streamPrint(spice_stream_, "+%.3e %.3e\n", time1, volt1);
}

float
WriteSpice::railToRailSlew(float slew,
                           const RiseFall *rf)
{
  float lower = default_library_->slewLowerThreshold(rf);
  float upper = default_library_->slewUpperThreshold(rf);
  return slew / (upper - lower);
}

////////////////////////////////////////////////////////////////

// Find the logic values for expression inputs to enable paths from input_port.
void
WriteSpice::gatePortValues(const Pin *input_pin,
                           const Pin *drvr_pin,
                           const RiseFall *drvr_rf,
                           const Edge *gate_edge,
                           // Return values.
                           LibertyPortLogicValues &port_values,
                           bool &is_clked)
{
  is_clked = false;
  const Instance *inst = network_->instance(input_pin);
  const LibertyPort *input_port = network_->libertyPort(input_pin);
  const LibertyPort *drvr_port = network_->libertyPort(drvr_pin);
  const FuncExpr *drvr_func = drvr_port->function();
  if (drvr_func) {
    if (gate_edge
        && gate_edge->role()->genericRole() == TimingRole::regClkToQ())
      regPortValues(input_pin, drvr_rf, drvr_port, drvr_func,
                    port_values, is_clked);
    else
      gatePortValues(inst, drvr_func, input_port, port_values);
  }
}

#if CUDD

void
WriteSpice::gatePortValues(const Instance *,
                           const FuncExpr *expr,
                           const LibertyPort *input_port,
                           // Return values.
                           LibertyPortLogicValues &port_values)
{
  DdNode *bdd = bdd_.funcBdd(expr);
  DdNode *input_node = bdd_.findNode(input_port);
  unsigned input_node_index = Cudd_NodeReadIndex(input_node);
  DdManager *cudd_mgr = bdd_.cuddMgr();
  DdNode *diff = Cudd_bddBooleanDiff(cudd_mgr, bdd, input_node_index);
  int *cube;
  CUDD_VALUE_TYPE value;
  DdGen *cube_gen = Cudd_FirstCube(cudd_mgr, diff, &cube, &value);

  FuncExprPortIterator port_iter(expr);
  while (port_iter.hasNext()) {
    const LibertyPort *port = port_iter.next();
    if (port != input_port) {
      DdNode *port_node = bdd_.findNode(port);
      int var_index = Cudd_NodeReadIndex(port_node);
      LogicValue value;
      switch (cube[var_index]) {
      case 0:
        value = LogicValue::zero;
        break;
      case 1:
        value = LogicValue::one;
        break;
      case 2:
      default:
        value = LogicValue::unknown;
        break;
      }
      port_values[port] = value;
    }
  }
  Cudd_GenFree(cube_gen);
  Cudd_Ref(diff);
  bdd_.clearVarMap();
}

#else

void
WriteSpice::gatePortValues(const Instance *inst,
                           const FuncExpr *expr,
                           const LibertyPort *input_port,
                           // Return values.
                           LibertyPortLogicValues &port_values)
{
  FuncExpr *left = expr->left();
  FuncExpr *right = expr->right();
  switch (expr->op()) {
  case FuncExpr::op_port:
    break;
  case FuncExpr::op_not:
    gatePortValues(inst, left, input_port, port_values);
    break;
  case FuncExpr::op_or:
    if (left->hasPort(input_port)
	&& right->op() == FuncExpr::op_port) {
      gatePortValues(inst, left, input_port, port_values);
      port_values[right->port()] = LogicValue::zero;
    }
    else if (left->hasPort(input_port)
	     && right->op() == FuncExpr::op_not
	     && right->left()->op() == FuncExpr::op_port) {
      // input_port + !right_port
      gatePortValues(inst, left, input_port, port_values);
      port_values[right->left()->port()] = LogicValue::one;
    }
    else if (right->hasPort(input_port)
	     && left->op() == FuncExpr::op_port) {
      gatePortValues(inst, right, input_port, port_values);
      port_values[left->port()] = LogicValue::zero;
    }
    else if (right->hasPort(input_port)
	     && left->op() == FuncExpr::op_not
	     && left->left()->op() == FuncExpr::op_port) {
      // input_port + !left_port
      gatePortValues(inst, right, input_port, port_values);
      port_values[left->left()->port()] = LogicValue::one;
    }
    else {
      gatePortValues(inst, left, input_port, port_values);
      gatePortValues(inst, right, input_port, port_values);
    }
    break;
  case FuncExpr::op_and:
    if (left->hasPort(input_port)
	&& right->op() == FuncExpr::op_port) {
      gatePortValues(inst, left, input_port, port_values);
      port_values[right->port()] = LogicValue::one;
    }
    else if (left->hasPort(input_port)
	     && right->op() == FuncExpr::op_not
	     && right->left()->op() == FuncExpr::op_port) {
      // input_port * !right_port
      gatePortValues(inst, left, input_port, port_values);
      port_values[right->left()->port()] = LogicValue::zero;
    }
    else if (right->hasPort(input_port)
	     && left->op() == FuncExpr::op_port) {
      gatePortValues(inst, right, input_port, port_values);
      port_values[left->port()] = LogicValue::one;
    }
    else if (right->hasPort(input_port)
	     && left->op() == FuncExpr::op_not
	     && left->left()->op() == FuncExpr::op_port) {
      // input_port * !left_port
      gatePortValues(inst, right, input_port, port_values);
      port_values[left->left()->port()] = LogicValue::zero;
    }
    else {
      gatePortValues(inst, left, input_port, port_values);
      gatePortValues(inst, right, input_port, port_values);
    }
    break;
  case FuncExpr::op_xor:
    // Need to know timing arc sense to get this right.
    if (left->port() == input_port
	&& right->op() == FuncExpr::op_port)
      port_values[right->port()] = LogicValue::zero;
    else if (right->port() == input_port
	     && left->op() == FuncExpr::op_port)
      port_values[left->port()] = LogicValue::zero;
    else {
      gatePortValues(inst, left, input_port, port_values);
      gatePortValues(inst, right, input_port, port_values);
    }
    break;
  case FuncExpr::op_one:
  case FuncExpr::op_zero:
    break;
  }
}

#endif

void
WriteSpice::regPortValues(const Pin *input_pin,
                          const RiseFall *drvr_rf,
                          const LibertyPort *drvr_port,
                          const FuncExpr *drvr_func,
                          // Return values.
                          LibertyPortLogicValues &port_values,
                          bool &is_clked)
{
  is_clked = false;
  LibertyPort *q_port = drvr_func->port();
  if (q_port) {
    // Drvr (register/latch output) function should be a reference
    // to an internal port like IQ or IQN.
    LibertyCell *cell = drvr_port->libertyCell();
    Sequential *seq = cell->outputPortSequential(q_port);
    if (seq) {
      seqPortValues(seq, drvr_rf, port_values);
      is_clked = true;
    }
    else {
      const LibertyPort *input_port = network_->libertyPort(input_pin);
      report_->error(1604, "no register/latch found for path from %s to %s,",
                     input_port->name(),
                     drvr_port->name());
    }
  }
}

void
WriteSpice::seqPortValues(Sequential *seq,
                          const RiseFall *rf,
                          // Return values.
                          LibertyPortLogicValues &port_values)
{
  FuncExpr *data = seq->data();
  LibertyPort *port = onePort(data);
  if (port) {
    TimingSense sense = data->portTimingSense(port);
    switch (sense) {
    case TimingSense::positive_unate:
      if (rf == RiseFall::rise())
	port_values[port] = LogicValue::rise;
      else
	port_values[port] = LogicValue::fall;
      break;
    case TimingSense::negative_unate:
      if (rf == RiseFall::rise())
	port_values[port] = LogicValue::fall;
      else
	port_values[port] = LogicValue::rise;
      break;
    case TimingSense::non_unate:
    case TimingSense::none:
    case TimingSense::unknown:
    default:
      break;
    }
  }
}

// Pick a port, any port...
LibertyPort *
WriteSpice::onePort(FuncExpr *expr)
{
  FuncExpr *left = expr->left();
  FuncExpr *right = expr->right();
  LibertyPort *port;
  switch (expr->op()) {
  case FuncExpr::op_port:
    return expr->port();
  case FuncExpr::op_not:
    return onePort(left);
  case FuncExpr::op_or:
  case FuncExpr::op_and:
  case FuncExpr::op_xor:
    port = onePort(left);
    if (port == nullptr)
      port = onePort(right);
    return port;
  case FuncExpr::op_one:
  case FuncExpr::op_zero:
  default:
    return nullptr;
  }
}

////////////////////////////////////////////////////////////////

PinSeq
WriteSpice::drvrLoads(const Pin *drvr_pin)
{
  PinSeq loads;
  Vertex *drvr_vertex = graph_->pinDrvrVertex(drvr_pin);
  VertexOutEdgeIterator edge_iter(drvr_vertex, graph_);
  while (edge_iter.hasNext()) {
    Edge *wire_edge = edge_iter.next();
    if (wire_edge->isWire()) {
      Vertex *load_vertex = wire_edge->to(graph_);
      const Pin *load_pin = load_vertex->pin();
      loads.push_back(load_pin);
    }
  }
  return loads;
}

void
WriteSpice::writeSubcktInstLoads(const Pin *drvr_pin,
                                 const Pin *exclude)
{
  streamPrint(spice_stream_, "* Load pins\n");
  PinSeq drvr_loads = drvrLoads(drvr_pin);
  // Do not sensitize side load gates.
  LibertyPortLogicValues port_values;
  for (const Pin *load_pin : drvr_loads) {
    if (load_pin != exclude
	&& network_->direction(load_pin)->isAnyInput()
	&& !network_->isHierarchical(load_pin)
	&& !network_->isTopLevelPort(load_pin)) {
      writeSubcktInst(load_pin);
      writeSubcktInstVoltSrcs(load_pin, port_values, nullptr, 0);
      streamPrint(spice_stream_, "\n");
    }
  }
}

////////////////////////////////////////////////////////////////

void
WriteSpice::writeMeasureDelayStmt(const Pin *from_pin,
                                  const RiseFall *from_rf,
                                  const Pin *to_pin,
                                  const RiseFall *to_rf,
                                  string prefix)
{
  const char *from_pin_name = network_->pathName(from_pin);
  float from_threshold = power_voltage_ * default_library_->inputThreshold(from_rf);
  const char *to_pin_name = network_->pathName(to_pin);
  float to_threshold = power_voltage_ * default_library_->inputThreshold(to_rf);
  streamPrint(spice_stream_,
	      ".measure tran %s_%s_delay_%s\n",
	      prefix.c_str(),
	      from_pin_name,
	      to_pin_name);
  streamPrint(spice_stream_,
	      "+trig v(%s) val=%.3f %s=last\n",
	      from_pin_name,
	      from_threshold,
	      spiceTrans(from_rf));
  streamPrint(spice_stream_,
	      "+targ v(%s) val=%.3f %s=last\n",
	      to_pin_name,
	      to_threshold,
	      spiceTrans(to_rf));
}

void
WriteSpice::writeMeasureSlewStmt(const Pin *pin,
                                 const RiseFall *rf,
                                 string prefix)
{
  const char *pin_name = network_->pathName(pin);
  const char *spice_rf = spiceTrans(rf);
  float lower = power_voltage_ * default_library_->slewLowerThreshold(rf);
  float upper = power_voltage_ * default_library_->slewUpperThreshold(rf);
  float threshold1, threshold2;
  if (rf == RiseFall::rise()) {
    threshold1 = lower;
    threshold2 = upper;
  }
  else {
    threshold1 = upper;
    threshold2 = lower;
  }
  streamPrint(spice_stream_,
	      ".measure tran %s_%s_slew\n",
	      prefix.c_str(),
	      pin_name);
  streamPrint(spice_stream_,
	      "+trig v(%s) val=%.3f %s=last\n",
	      pin_name,
	      threshold1,
	      spice_rf);
  streamPrint(spice_stream_,
	      "+targ v(%s) val=%.3f %s=last\n",
	      pin_name,
	      threshold2,
	      spice_rf);
}

////////////////////////////////////////////////////////////////


const char *
WriteSpice::spiceTrans(const RiseFall *rf)
{
  if (rf == RiseFall::rise())
    return "RISE";
  else
    return "FALL";
}

// fprintf for c++ streams.
// Yes, I hate formatted output to ostream THAT much.
void
streamPrint(ofstream &stream,
	    const char *fmt,
	    ...)
{
  va_list args;
  va_start(args, fmt);
  char *result = nullptr;
  if (vasprintf(&result, fmt, args) == -1)
    criticalError(267, "out of memory");
  stream << result;
  free(result);
  va_end(args);
}

}
