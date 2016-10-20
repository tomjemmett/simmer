#include "process.h"
#include "simulator.h"
#include "activity.h"

bool Process::activate(double delay) {
  sim->schedule(delay, this, priority);
  active = true;
  return true;
}

bool Process::deactivate() {
  if (!active) return false;
  sim->unschedule(this);
  active = false;
  return true;
}

void Generator::set_first_activity() {
  Rcpp::Function get_head(trj["get_head"]);
  first_activity = Rcpp::as<Rcpp::XPtr<Activity> >(get_head());
}

void Generator::run() {
  // get the delay for the next (n) arrival(s)
  Rcpp::NumericVector delays = dist();
  int n = delays.size();
  double delay = 0;

  for(int i = 0; i < n; ++i) {
    if (delays[i] < 0) {
      active = false;
      return;
    }
    delay += delays[i];

    // format the name and create the next arrival
    std::string arr_name = name + boost::lexical_cast<std::string>(count++);
    Arrival* arrival = new Arrival(sim, arr_name, is_monitored(),
                                   order, first_activity, count);

    if (sim->verbose)Rcpp::Rcout <<
      FMT(10, right) << sim->now() << " |" <<
      FMT(12, right) << "generator: " << FMT(15, left) << name << "|" <<
      FMT(12, right) << "new: " << FMT(15, left) << arr_name << "| " <<
      (sim->now() + delay) << std::endl;

    // schedule the arrival
    sim->register_arrival(arrival);
    sim->schedule(delay, arrival,
                  first_activity->priority ? first_activity->priority : count);
  }
  // schedule the generator
  activate(delay);
}

void Manager::run() {
  if (sim->verbose) Rcpp::Rcout <<
    FMT(10, right) << sim->now() << " |" <<
    FMT(12, right) << "manager: " << FMT(15, left) << name << "|" <<
    FMT(12, right) << "parameter: " << FMT(15, left) << param << "| " <<
    value[index] << std::endl;

  set(value[index]);
  index++;
  if (index == duration.size()) {
    if (period < 0)
      goto end;
    index = 1;
  }

  activate();
end:
  return;
}

void Task::run() {
  if (sim->verbose) Rcpp::Rcout <<
    FMT(10, right) << sim->now() << " |" <<
    FMT(12, right) << "task: " << FMT(15, left) << name << "|" <<
    FMT(12, right) << " " << FMT(15, left) << " " << "| " << std::endl;

  task();
  delete this;
}

void Arrival::reset() {
  cancel_timeout();
  if (!--(*clones))
    delete clones;
  sim->unregister_arrival(this);
}

void Arrival::run() {
  double delay;

  if (!activity)
    goto finish;
  if (lifetime.start < 0)
    lifetime.start = sim->now();

  if (sim->verbose) {
    Rcpp::Rcout <<
      FMT(10, right) << sim->now() << " |" <<
      FMT(12, right) << "arrival: " << FMT(15, left) << name << "|" <<
      FMT(12, right) << "activity: " << FMT(15, left) << activity->name << "| ";
    activity->print(0, true);
  }

  active = false;
  delay = activity->run(this);
  if (delay == REJECT)
    goto end;
  activity = activity->get_next();
  if (delay == ENQUEUE)
    goto end;
  active = true;

  if (delay != BLOCK) {
    set_busy(sim->now() + delay);
    update_activity(delay);
  }
  sim->schedule(delay, this, activity ? activity->priority : priority);
  goto end;

finish:
  terminate(true);
end:
  return;
}

void Arrival::restart() {
  set_busy(sim->now() + lifetime.remaining);
  activate(lifetime.remaining);
  set_remaining(0);
}

void Arrival::interrupt() {
  deactivate();
  if (lifetime.busy_until < sim->now())
    return;
  unset_busy(sim->now());
  if (lifetime.remaining && order.get_restart()) {
    unset_remaining();
    activity = activity->get_prev();
  }
}

void Arrival::leave(std::string resource) {
  sim->record_release(name, restime[resource].start, restime[resource].activity, resource);
}

void Arrival::leave(std::string resource, double start, double activity) {
  sim->record_release(name, start, activity, resource);
}

void Arrival::terminate(bool finished) {
  foreach_ (ResMSet::value_type& itr, resources) {
    Rcpp::warning("`%s`: leaving without releasing `%s`", name, itr->name);
    itr->erase(this, true);
  }
  unset_remaining();
  if (is_monitored() >= 1 && !dynamic_cast<Batched*>(this))
    sim->record_end(name, lifetime.start, lifetime.activity, finished);
  delete this;
}

void Arrival::renege(Activity* next) {
  bool ret = false;
  timer = NULL;
  if (batch) {
    if (batch->is_permanent())
      return;
    ret = true;
    batch->erase(this);
  }
  if (lifetime.busy_until > sim->now())
    unset_busy(sim->now());
  unset_remaining();
  while (resources.begin() != resources.end())
    ret |= (*resources.begin())->erase(this);
  if (!ret)
    deactivate();
  if (next) {
    activity = next;
    activate();
  } else terminate(false);
}

int Arrival::set_attribute(std::string key, double value) {
  attributes[key] = value;
  if (is_monitored() >= 2)
    sim->record_attribute(name, key, value);
  return 0;
}

double Arrival::get_start(std::string name) {
  double start = restime[name].start;
  if (batch) {
    double up = batch->get_start(name);
    if (up >= 0 && (start < 0 || up < start))
      start = up;
  }
  return start;
}

void Arrival::register_entity(Resource* ptr) {
  if (is_monitored())
      restime[ptr->name].start = sim->now();
  resources.insert(ptr);
}

void Arrival::unregister_entity(Resource* ptr) {
  if (is_monitored())
    leave(ptr->name);
  resources.erase(resources.find(ptr));
}

void Batched::terminate(bool finished) {
  foreach_ (Arrival* arrival, arrivals)
    arrival->terminate(finished);
  arrivals.clear();
  Arrival::terminate(finished);
}

int Batched::set_attribute(std::string key, double value) {
  attributes[key] = value;
  foreach_ (Arrival* arrival, arrivals)
    arrival->set_attribute(key, value);
  return 0;
}

void Batched::erase(Arrival* arrival) {
  bool del = activity;
  if (arrivals.size() > 1 || (batch && batch->is_permanent())) {
    del = false;
    if (arrival->is_monitored()) {
      Batched* up = this;
      while (up) {
        up->report(arrival);
        up = up->batch;
      }
    }
  } else if (arrivals.size() == 1 && !batch) {
    bool ret = !activity;
    if (lifetime.busy_until > sim->now())
      unset_busy(sim->now());
    unset_remaining();
    while (resources.begin() != resources.end())
      (*resources.begin())->erase(this);
    if (!ret)
      deactivate();
  } else {
    batch->erase(this);
    if (lifetime.busy_until > sim->now())
      unset_busy(sim->now());
    unset_remaining();
    while (resources.begin() != resources.end())
      (*resources.begin())->erase(this);
  }
  arrivals.erase(std::remove(arrivals.begin(), arrivals.end(), arrival), arrivals.end());
  arrival->unregister_entity(this);
  if (del) delete this;
}

void Batched::report(Arrival* arrival) {
  foreach_ (ResTime::value_type& itr, restime)
    arrival->leave(itr.first, itr.second.start,
                   itr.second.activity - lifetime.busy_until + sim->now());
}
